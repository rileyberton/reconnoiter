
/*
 * Copyright (c) 2007, OmniTI Computer Consulting, Inc.
 * All rights reserved.
 */

#include "noit_defines.h"

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <assert.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xpath.h>

#include "noit_conf.h"
#include "noit_conf_private.h"
#include "noit_check.h"
#include "noit_filters.h"
#include "noit_console.h"
#include "utils/noit_hash.h"
#include "utils/noit_log.h"

static void register_console_config_commands();
static int noit_console_config_cd(noit_console_closure_t ncct,
                                  int argc, char **argv,
                                  noit_console_state_t *state, void *closure);

static struct _valid_attr_t {
  const char *scope;
  const char *name;
  const char *xpath;
  int checks_fixate;
} valid_attrs[] = {
  { "/checks", "name", "@name", 0 },
  { "/checks", "target", "@target", 0 },
  { "/checks", "period", "@period", 0 },
  { "/checks", "timeout", "@timeout", 0 },
  { "/checks", "oncheck", "@oncheck", 0 },
  { "/checks", "disable", "@disable", 0 },
  { "/checks", "filterset", "@filterset", 0 },
  { "/checks", "module", "@module", 1 },
  { "/filtersets/filterset", "target", "@target", 1 },
  { "/filtersets/filterset", "module", "@module", 1 },
  { "/filtersets/filterset", "name", "@name", 1 },
  { "/filtersets/filterset", "metric", "@metric", 1 },
};

void
noit_console_state_add_check_attrs(noit_console_state_t *state,
                                   console_cmd_func_t f) {
  int i;
  for(i = 0;
      i < sizeof(valid_attrs)/sizeof(valid_attrs[0]);
      i++) {
    noit_console_state_add_cmd(state,
      NCSCMD(valid_attrs[i].name, f, NULL,
             NULL, &valid_attrs[i]));
  }
}
static noit_hash_table check_attrs = NOIT_HASH_EMPTY;
void noit_conf_checks_init(const char *toplevel) {
  int i;
  for(i=0;i<sizeof(valid_attrs)/sizeof(*valid_attrs);i++) {
    noit_hash_store(&check_attrs,
                    valid_attrs[i].name, strlen(valid_attrs[i].name),
                    &valid_attrs[i]);
  }
  register_console_config_commands();
}

static void
conf_t_userdata_free(void *data) {
  noit_conf_t_userdata_t *info = data;
  if(info) {
    if(info->path) free(info->path);
    free(info);
  }
}

static int
noit_console_mkcheck_xpath(char *xpath, int len,
                           noit_conf_t_userdata_t *info,
                           const char *arg) {
  uuid_t checkid;
  char argcopy[1024], *target, *module, *name;

  argcopy[0] = '\0';
  if(arg) strlcpy(argcopy, arg, sizeof(argcopy));

  if(uuid_parse(argcopy, checkid) == 0) {
    /* If they kill by uuid, we'll seek and destroy -- find it anywhere */
    snprintf(xpath, len, "/noit/checks//check[@uuid=\"%s\"]",
             argcopy);
  }
  else if((module = strchr(argcopy, '`')) != NULL) {
    noit_check_t *check;
    char uuid_str[37];
    target = argcopy;
    *module++ = '\0';
    if((name = strchr(module+1, '`')) == NULL)
      name = module;
    else
      name++;
    check = noit_poller_lookup_by_name(target, name);
    if(!check) {
      return -1;
    }
    uuid_unparse_lower(check->checkid, uuid_str);
    snprintf(xpath, len, "/noit/checks//check[@uuid=\"%s\"]",
             uuid_str);
  }
  else {
    char *path = (!info || !strcmp(info->path, "/")) ? "" : info->path;
    snprintf(xpath, len, "/noit%s%s%s[@uuid]",
             path, arg ? "/" : "", arg ? arg : "");
  }
  return 0;
}
static void
nc_attr_show(noit_console_closure_t ncct, const char *name, xmlNodePtr cnode,
             xmlNodePtr anode, const char *value) {
  const char *cpath, *apath;
  cpath = cnode ? (char *)xmlGetNodePath(cnode) : "";
  apath = anode ? (char *)xmlGetNodePath(anode) : "";
  nc_printf(ncct, " %s: %s", name, value ? value : "[undef]");
  if(value && cpath && apath) {
    int clen = strlen(cpath);
    int plen = strlen("/noit/checks/");
    if(!strncmp(cpath, apath, clen) && apath[clen] == '/') {
      /* we have a match, which means it isn't inherited */
    }
    else {
      nc_printf(ncct, " [inherited from %s]",
                strlen(apath) > plen ? apath + plen : apath);
    }
  }
  nc_write(ncct, "\n", 1);
}
static void 
refresh_subchecks(noit_console_closure_t ncct,
                  noit_conf_t_userdata_t *info) {
  char *path;
  char xpath[1024];
 
  path = info->path;
  if(!strcmp(path, "/")) path = "";

  /* The first one is just a process_checks, the second is the reload.
   * Reload does a lot of work and there is no need to do it twice.
   */
  snprintf(xpath, sizeof(xpath), "/noit/%s[@uuid]", path);
  noit_poller_process_checks(xpath);
  snprintf(xpath, sizeof(xpath), "/noit/%s//check[@uuid]", path);
  noit_poller_reload(xpath);
}
static int
noit_config_check_update_attrs(xmlNodePtr node, int argc, char **argv) {
  int i, error = 0;
  if(argc % 2) return -1;

  for(i=0; i<argc; i+=2) {
    struct _valid_attr_t *attrinfo;
    char *attr = argv[i], *val = NULL;
    if(!strcasecmp(argv[i], "no")) attr = argv[i+1];
    else val = argv[i+1];
    if(!noit_hash_retrieve(&check_attrs, attr, strlen(attr),
                           (void **)&attrinfo)) {
      error = 1;
      break;
    }
    /* The fixation stuff doesn't matter here, this check is brand-new */
    xmlUnsetProp(node, (xmlChar *)attrinfo->name);
    if(val)
      xmlSetProp(node, (xmlChar *)attrinfo->name, (xmlChar *)val);
    noit_conf_mark_changed();
  }
  return error;
}

static int
noit_conf_mkcheck_under(const char *ppath, int argc, char **argv, uuid_t out) {
  int rv = -1;
  const char *path;
  char xpath[1024];
  xmlXPathContextPtr xpath_ctxt = NULL;
  xmlXPathObjectPtr pobj = NULL;
  xmlNodePtr node = NULL, newnode;

  /* attr val [or] no attr (sets of two) */
  if(argc % 2) goto out;

  noit_conf_xml_xpath(NULL, &xpath_ctxt);
  path = strcmp(ppath, "/") ? ppath : "";
  snprintf(xpath, sizeof(xpath), "/noit%s", path);
  pobj = xmlXPathEval((xmlChar *)xpath, xpath_ctxt);
  if(!pobj || pobj->type != XPATH_NODESET ||
     xmlXPathNodeSetGetLength(pobj->nodesetval) != 1) {
    goto out;
  }
  node = (noit_conf_section_t)xmlXPathNodeSetItem(pobj->nodesetval, 0);
  if((newnode = xmlNewChild(node, NULL, (xmlChar *)"check", NULL)) != NULL) {
    char outstr[37];
    uuid_generate(out);
    uuid_unparse_lower(out, outstr);
    xmlSetProp(newnode, (xmlChar *)"uuid", (xmlChar *)outstr);
    xmlSetProp(newnode, (xmlChar *)"disable", (xmlChar *)"true");

    /* No risk of running off the end (we checked this above) */
    if(noit_config_check_update_attrs(newnode, argc, argv)) {
      /* Something went wrong, remove the node */
      xmlUnlinkNode(newnode);
    }
    else {
      noit_conf_mark_changed();
      rv = 0;
    }
  }
 out:
  if(pobj) xmlXPathFreeObject(pobj);
  return rv;
}

static int
noit_console_check(noit_console_closure_t ncct,
                   int argc, char **argv,
                   noit_console_state_t *state, void *closure) {
  int cnt;
  noit_conf_t_userdata_t *info;
  char xpath[1024], newuuid_str[37];
  char *uuid_conf, *wanted;
  uuid_t checkid;
  xmlXPathContextPtr xpath_ctxt = NULL;
  xmlXPathObjectPtr pobj = NULL;
  xmlNodePtr node = NULL;
  noit_boolean creating_new = noit_false;

  if(closure) {
    char *fake_argv[1] = { ".." };
    noit_console_state_pop(ncct, 0, argv, NULL, NULL);
    noit_console_config_cd(ncct, 1, fake_argv, NULL, NULL);
  }

  noit_conf_xml_xpath(NULL, &xpath_ctxt);
  if(argc < 1) {
    nc_printf(ncct, "requires at least one argument\n");
    return -1;
  }
  if(argc % 2 == 0) {
    nc_printf(ncct, "wrong number of arguments\n");
    return -1;
  } 

  info = noit_console_userdata_get(ncct, NOIT_CONF_T_USERDATA);
  wanted = strcmp(argv[0], "new") ? argv[0] : NULL;
  if(info && !wanted) {
    /* We are creating a new node */
    uuid_t out;
    creating_new = noit_true;
    if(noit_conf_mkcheck_under(info->path, argc - 1, argv + 1, out)) {
      nc_printf(ncct, "Error creating new check\n");
      return -1;
    }
    newuuid_str[0] = '\0';
    uuid_unparse_lower(out, newuuid_str);
    wanted = newuuid_str;
  }
  /* We many not be in conf-t mode -- that's fine */
  if(noit_console_mkcheck_xpath(xpath, sizeof(xpath), info, wanted)) {
    nc_printf(ncct, "could not find check '%s'\n", wanted);
    return -1;
  }

  pobj = xmlXPathEval((xmlChar *)xpath, xpath_ctxt);
  if(!pobj || pobj->type != XPATH_NODESET ||
     xmlXPathNodeSetIsEmpty(pobj->nodesetval)) {
    nc_printf(ncct, "no checks found for '%s'\n", wanted);
    goto out;
  }
  cnt = xmlXPathNodeSetGetLength(pobj->nodesetval);
  if(info && cnt != 1) {
    nc_printf(ncct, "Ambiguous check specified\n");
    goto out;
  }
  node = (noit_conf_section_t)xmlXPathNodeSetItem(pobj->nodesetval, 0);
  uuid_conf = (char *)xmlGetProp(node, (xmlChar *)"uuid");
  if(!uuid_conf || uuid_parse(uuid_conf, checkid)) {
    nc_printf(ncct, "%s has invalid or missing UUID!\n",
              (char *)xmlGetNodePath(node) + strlen("/noit"));
    goto out;
  }
  if(argc > 1 && !creating_new)
    if(noit_config_check_update_attrs(node, argc - 1, argv + 1))
      nc_printf(ncct, "Partially successful, error setting some attributes\n");

  if(info) {
    if(info->path) free(info->path);
    info->path = strdup((char *)xmlGetNodePath(node) + strlen("/noit"));
    uuid_copy(info->current_check, checkid);
    if(argc > 1) refresh_subchecks(ncct, info);
    if(state) {
      noit_console_state_push_state(ncct, state);
      noit_console_state_init(ncct);
    }
    goto out;
  }
 out:
  if(pobj) xmlXPathFreeObject(pobj);
  return 0;
}
static int
noit_console_watch_check(noit_console_closure_t ncct,
                         int argc, char **argv,
                         noit_console_state_t *state, void *closure) {
  int i, cnt;
  int adding = (int)closure;
  int period = 0;
  char xpath[1024];
  xmlXPathObjectPtr pobj = NULL;
  xmlXPathContextPtr xpath_ctxt = NULL;

  noit_conf_xml_xpath(NULL, &xpath_ctxt);
  if(argc < 1 || argc > 2) {
    nc_printf(ncct, "requires one or two arguments\n");
    return -1;
  }
  /* An alternate period */
  if(argc == 2) period = atoi(argv[1]);

  if(noit_console_mkcheck_xpath(xpath, sizeof(xpath), NULL,
                                argc ? argv[0] : NULL)) {
    nc_printf(ncct, "ERROR: could not find check '%s'\n", argv[0]);
    return -1;
  }

  pobj = xmlXPathEval((xmlChar *)xpath, xpath_ctxt);
  if(!pobj || pobj->type != XPATH_NODESET ||
     xmlXPathNodeSetIsEmpty(pobj->nodesetval)) {
    nc_printf(ncct, "no checks found\n");
    goto out;
  }
  cnt = xmlXPathNodeSetGetLength(pobj->nodesetval);
  for(i=0; i<cnt; i++) {
    uuid_t checkid;
    noit_check_t *check;
    xmlNodePtr node;
    char *uuid_conf;

    node = (noit_conf_section_t)xmlXPathNodeSetItem(pobj->nodesetval, i);
    uuid_conf = (char *)xmlGetProp(node, (xmlChar *)"uuid");
    if(!uuid_conf || uuid_parse(uuid_conf, checkid)) {
      nc_printf(ncct, "%s has invalid or missing UUID!\n",
                (char *)xmlGetNodePath(node) + strlen("/noit"));
      continue;
    }
    if(period == 0) {
      check = noit_poller_lookup(checkid);
      if(!check) continue;
      if(adding) noit_check_transient_add_feed(check, ncct->feed_path);
      else noit_check_transient_remove_feed(check, ncct->feed_path);
    }
    else {
      if(adding) {
        check = noit_check_watch(checkid, period);
        /* This check must be watched from the console */
        noit_check_transient_add_feed(check, ncct->feed_path);
        /* Note the check */
        noit_check_log_check(check);
        /* kick it off, if it isn't running already */
        if(!NOIT_CHECK_LIVE(check)) noit_check_activate(check);
      }
      else {
        check = noit_check_get_watch(checkid, period);
        if(check) noit_check_transient_remove_feed(check, ncct->feed_path);
      }
    }
  }
 out:
  if(pobj) xmlXPathFreeObject(pobj);
  return 0;
}
static int
noit_console_show_check(noit_console_closure_t ncct,
                        int argc, char **argv,
                        noit_console_state_t *state, void *closure) {
  int i, cnt;
  noit_conf_t_userdata_t *info;
  char xpath[1024];
  xmlXPathObjectPtr pobj = NULL;
  xmlXPathContextPtr xpath_ctxt = NULL;

  noit_conf_xml_xpath(NULL, &xpath_ctxt);
  if(argc > 1) {
    nc_printf(ncct, "requires zero or one arguments\n");
    return -1;
  }

  info = noit_console_userdata_get(ncct, NOIT_CONF_T_USERDATA);
  /* We many not be in conf-t mode -- that's fine */
  if(noit_console_mkcheck_xpath(xpath, sizeof(xpath), info,
                                argc ? argv[0] : NULL)) {
    nc_printf(ncct, "could not find check '%s'\n", argv[0]);
    return -1;
  }

  pobj = xmlXPathEval((xmlChar *)xpath, xpath_ctxt);
  if(!pobj || pobj->type != XPATH_NODESET ||
     xmlXPathNodeSetIsEmpty(pobj->nodesetval)) {
    nc_printf(ncct, "no checks found\n");
    goto out;
  }
  cnt = xmlXPathNodeSetGetLength(pobj->nodesetval);
  if(info && cnt != 1) {
    nc_printf(ncct, "Ambiguous check specified\n");
    goto out;
  }
  for(i=0; i<cnt; i++) {
    noit_hash_iter iter = NOIT_HASH_ITER_ZERO;
    const char *k;
    int klen;
    void *data;
    uuid_t checkid;
    noit_check_t *check;
    noit_hash_table *config;
    xmlNodePtr node, anode, mnode = NULL;
    char *uuid_conf;
    char *module, *value;

    node = (noit_conf_section_t)xmlXPathNodeSetItem(pobj->nodesetval, i);
    uuid_conf = (char *)xmlGetProp(node, (xmlChar *)"uuid");
    if(!uuid_conf || uuid_parse(uuid_conf, checkid)) {
      nc_printf(ncct, "%s has invalid or missing UUID!\n",
                (char *)xmlGetNodePath(node) + strlen("/noit"));
      continue;
    }
    nc_printf(ncct, "==== %s ====\n", uuid_conf);

#define MYATTR(a,n,b) _noit_conf_get_string(node, &(n), "@" #a, &(b))
#define INHERIT(a,n,b) \
  _noit_conf_get_string(node, &(n), "ancestor-or-self::node()/@" #a, &(b))
#define SHOW_ATTR(a) do { \
  anode = NULL; \
  value = NULL; \
  INHERIT(a, anode, value); \
  nc_attr_show(ncct, #a, node, anode, value); \
} while(0)

    if(!INHERIT(module, mnode, module)) module = NULL;
    if(MYATTR(name, anode, value))
      nc_printf(ncct, " name: %s\n", value);
    else
      nc_printf(ncct, " name: %s [from module]\n", module ? module : "[undef]");
    nc_attr_show(ncct, "module", node, mnode, module);
    SHOW_ATTR(target);
    SHOW_ATTR(period);
    SHOW_ATTR(timeout);
    SHOW_ATTR(oncheck);
    SHOW_ATTR(filterset);
    SHOW_ATTR(disable);
    /* Print out all the config settings */
    config = noit_conf_get_hash(node, "config");
    while(noit_hash_next(config, &iter, &k, &klen, &data)) {
      nc_printf(ncct, " config::%s: %s\n", k, (const char *)data);
    }
    noit_hash_destroy(config, free, free);
    free(config);

    check = noit_poller_lookup(checkid);
    if(!check) {
      nc_printf(ncct, " ERROR: not in running system\n");
    }
    else {
      int idx = 0;
      nc_printf(ncct, " currently: ");
      if(NOIT_CHECK_RUNNING(check)) nc_printf(ncct, "%srunning", idx++?",":"");
      if(NOIT_CHECK_KILLED(check)) nc_printf(ncct, "%skilled", idx++?",":"");
      if(!NOIT_CHECK_CONFIGURED(check)) nc_printf(ncct, "%sunconfig", idx++?",":"");
      if(NOIT_CHECK_DISABLED(check)) nc_printf(ncct, "%sdisabled", idx++?",":"");
      if(!idx) nc_printf(ncct, "idle");
      nc_write(ncct, "\n", 1);
      if(check->stats.current.whence.tv_sec == 0) {
        nc_printf(ncct, " last run: never\n");
      }
      else {
        stats_t *c = &check->stats.current;
        struct timeval now, diff;
        gettimeofday(&now, NULL);
        sub_timeval(now, c->whence, &diff);
        nc_printf(ncct, " last run: %0.3f seconds ago\n",
                  diff.tv_sec + (diff.tv_usec / 1000000.0));
        nc_printf(ncct, " availability/state: %s/%s\n",
                  noit_check_available_string(c->available),
                  noit_check_state_string(c->state));
        nc_printf(ncct, " status: %s\n", c->status ? c->status : "[[null]]");
        nc_printf(ncct, " metrics:\n");
        memset(&iter, 0, sizeof(iter));
        while(noit_hash_next(&c->metrics, &iter, &k, &klen, &data)) {
          char buff[256];
          noit_stats_snprint_metric(buff, sizeof(buff), (metric_t *)data);
          nc_printf(ncct, "   %s\n", buff);
        }
      }
    }
  }
 out:
  if(pobj) xmlXPathFreeObject(pobj);
  return 0;
}
static int
noit_console_config_nocheck(noit_console_closure_t ncct,
                            int argc, char **argv,
                            noit_console_state_t *state, void *closure) {
  int i, cnt;
  const char *err = "internal error";
  noit_conf_t_userdata_t *info;
  xmlXPathObjectPtr pobj = NULL;
  xmlXPathContextPtr xpath_ctxt = NULL;
  char xpath[1024];
  uuid_t checkid;

  noit_conf_xml_xpath(NULL, &xpath_ctxt);
  if(argc < 1) {
    nc_printf(ncct, "requires one argument\n");
    return -1;
  }

  info = noit_console_userdata_get(ncct, NOIT_CONF_T_USERDATA);
  if(noit_console_mkcheck_xpath(xpath, sizeof(xpath), info, argv[0])) {
    nc_printf(ncct, "could not find check '%s'\n", argv[0]);
    return -1;
  }
  pobj = xmlXPathEval((xmlChar *)xpath, xpath_ctxt);
  if(!pobj || pobj->type != XPATH_NODESET ||
     xmlXPathNodeSetIsEmpty(pobj->nodesetval)) {
    err = "no checks found";
    goto bad;
  }
  cnt = xmlXPathNodeSetGetLength(pobj->nodesetval);
  for(i=0; i<cnt; i++) {
    xmlNodePtr node;
    char *uuid_conf;
    node = (noit_conf_section_t)xmlXPathNodeSetItem(pobj->nodesetval, i);
    uuid_conf = (char *)xmlGetProp(node, (xmlChar *)"uuid");
    if(!uuid_conf || uuid_parse(uuid_conf, checkid)) {
      nc_printf(ncct, "%s has invalid or missing UUID!\n",
                (char *)xmlGetNodePath(node) + strlen("/noit"));
    }
    else {
      if(argc > 1) {
        int j;
        for(j=1;j<argc;j++)
          xmlUnsetProp(node, (xmlChar *)argv[j]);
      } else {
        nc_printf(ncct, "descheduling %s\n", uuid_conf);
        noit_poller_deschedule(checkid);
        xmlUnlinkNode(node);
      }
      noit_conf_mark_changed();
    }
  }
  if(argc > 1) {
    noit_poller_process_checks(xpath);
    noit_poller_reload(xpath);
  }
  nc_printf(ncct, "rebuilding causal map...\n");
  noit_poller_make_causal_map();
  if(pobj) xmlXPathFreeObject(pobj);
  return 0;
 bad:
  if(pobj) xmlXPathFreeObject(pobj);
  nc_printf(ncct, "%s\n", err);
  return -1;
}
static int
noit_console_state_conf_terminal(noit_console_closure_t ncct,
                                 int argc, char **argv,
                                 noit_console_state_t *state, void *closure) {
  noit_conf_t_userdata_t *info;
  if(argc) {
    nc_printf(ncct, "extra arguments not expected.\n");
    return -1;
  }
  info = calloc(1, sizeof(*info));
  info->path = strdup("/");
  noit_console_userdata_set(ncct, NOIT_CONF_T_USERDATA, info,
                            conf_t_userdata_free);
  noit_console_state_push_state(ncct, state);
  noit_console_state_init(ncct);
  return 0;
}
static int
noit_console_config_section(noit_console_closure_t ncct,
                            int argc, char **argv,
                            noit_console_state_t *state, void *closure) {
  const char *err = "internal error";
  char *path, xpath[1024];
  noit_conf_t_userdata_t *info;
  xmlXPathObjectPtr pobj = NULL;
  xmlXPathContextPtr xpath_ctxt = NULL;
  xmlNodePtr node = NULL, newnode;
  vpsized_int delete = (vpsized_int)closure;

  noit_conf_xml_xpath(NULL, &xpath_ctxt);
  if(argc != 1) {
    nc_printf(ncct, "requires one argument\n");
    return -1;
  }
  if(strchr(argv[0], '/')) {
    nc_printf(ncct, "invalid section name\n");
    return -1;
  }
  if(!strcmp(argv[0], "check")) {
    nc_printf(ncct, "use 'check' to create checks\n");
    return -1;
  }
  if(!strcmp(argv[0], "config")) {
    nc_printf(ncct, "use 'config' to set check config options\n");
    return -1;
  }
  info = noit_console_userdata_get(ncct, NOIT_CONF_T_USERDATA);
  if(!strcmp(info->path, "/")) {
    nc_printf(ncct, "manipulation of toplevel section disallowed\n");
    return -1;
  }

  if(delete) {
    /* We cannot delete if we have checks */
    snprintf(xpath, sizeof(xpath), "/noit%s/%s//check", info->path, argv[0]);
    pobj = xmlXPathEval((xmlChar *)xpath, xpath_ctxt);
    if(!pobj || pobj->type != XPATH_NODESET ||
       !xmlXPathNodeSetIsEmpty(pobj->nodesetval)) {
      err = "cannot delete section, has checks";
      goto bad;
    }
    if(pobj) xmlXPathFreeObject(pobj);
  }

  snprintf(xpath, sizeof(xpath), "/noit%s/%s", info->path, argv[0]);
  pobj = xmlXPathEval((xmlChar *)xpath, xpath_ctxt);
  if(!pobj || pobj->type != XPATH_NODESET) {
    err = "internal error: cannot detect section";
    goto bad;
  }
  if(!delete && !xmlXPathNodeSetIsEmpty(pobj->nodesetval)) {
    if(xmlXPathNodeSetGetLength(pobj->nodesetval) == 1) {
      node = xmlXPathNodeSetItem(pobj->nodesetval, 0);
      if(info->path) free(info->path);
      info->path = strdup((char *)xmlGetNodePath(node) + strlen("/noit"));
      goto cdout;
    }
    err = "cannot create section";
    goto bad;
  }
  if(delete && xmlXPathNodeSetIsEmpty(pobj->nodesetval)) {
    err = "no such section";
    goto bad;
  }
  if(delete) {
    node = (noit_conf_section_t)xmlXPathNodeSetItem(pobj->nodesetval, 0);
    xmlUnlinkNode(node);
    noit_conf_mark_changed();
    return 0;
  }
  if(pobj) xmlXPathFreeObject(pobj);

  path = strcmp(info->path, "/") ? info->path : "";
  snprintf(xpath, sizeof(xpath), "/noit%s", path);
  pobj = xmlXPathEval((xmlChar *)xpath, xpath_ctxt);
  if(!pobj || pobj->type != XPATH_NODESET ||
     xmlXPathNodeSetGetLength(pobj->nodesetval) != 1) {
    err = "path invalid?";
    goto bad;
  }
  node = (noit_conf_section_t)xmlXPathNodeSetItem(pobj->nodesetval, 0);
  if((newnode = xmlNewChild(node, NULL, (xmlChar *)argv[0], NULL)) != NULL) {
    noit_conf_mark_changed();
    if(info->path) free(info->path);
    info->path = strdup((char *)xmlGetNodePath(newnode) + strlen("/noit"));
  }
  else {
    err = "failed to create section";
    goto bad;
  }
 cdout:
  if(pobj) xmlXPathFreeObject(pobj);
  return 0;
 bad:
  if(pobj) xmlXPathFreeObject(pobj);
  nc_printf(ncct, "%s\n", err);
  return -1;
}

static int
noit_console_config_cd(noit_console_closure_t ncct,
                       int argc, char **argv,
                       noit_console_state_t *state, void *closure) {
  const char *err = "internal error";
  char *path, xpath[1024];
  noit_conf_t_userdata_t *info;
  xmlXPathObjectPtr pobj = NULL;
  xmlXPathContextPtr xpath_ctxt = NULL, current_ctxt;
  xmlNodePtr node = NULL;
  char *dest;

  noit_conf_xml_xpath(NULL, &xpath_ctxt);
  if(argc != 1 && !closure) {
    nc_printf(ncct, "requires one argument\n");
    return -1;
  }
  dest = argc ? argv[0] : (char *)closure;
  info = noit_console_userdata_get(ncct, NOIT_CONF_T_USERDATA);
  if(dest[0] == '/')
    snprintf(xpath, sizeof(xpath), "/noit%s", dest);
  else {
    snprintf(xpath, sizeof(xpath), "/noit%s/%s", info->path, dest);
  }
  if(xpath[strlen(xpath)-1] == '/') xpath[strlen(xpath)-1] = '\0';

  current_ctxt = xpath_ctxt;
  pobj = xmlXPathEval((xmlChar *)xpath, current_ctxt);
  if(!pobj || pobj->type != XPATH_NODESET ||
     xmlXPathNodeSetIsEmpty(pobj->nodesetval)) {
    err = "no such section";
    goto bad;
  }
  if(xmlXPathNodeSetGetLength(pobj->nodesetval) > 1) {
    err = "ambiguous section";
    goto bad;
  }

  node = (noit_conf_section_t)xmlXPathNodeSetItem(pobj->nodesetval, 0);
  if(!strcmp((char *)node->name, "check")) {
    err = "can't cd into a check, use 'check' instead";
    goto bad;
  }
  path = (char *)xmlGetNodePath(node);
  if(strncmp(path, "/noit/", strlen("/noit/")) && strcmp(path, "/noit")) {
    err = "new path outside out tree";
    goto bad;
  }
  free(info->path);
  if(!strcmp(path, "/noit"))
    info->path = strdup("/");
  else
    info->path = strdup((char *)xmlGetNodePath(node) + strlen("/noit"));
  if(pobj) xmlXPathFreeObject(pobj);
  if(closure) noit_console_state_pop(ncct, argc, argv, NULL, NULL);
  return 0;
 bad:
  if(pobj) xmlXPathFreeObject(pobj);
  nc_printf(ncct, "%s [%s]\n", err, xpath);
  return -1;
}
static int
noit_console_config_show(noit_console_closure_t ncct,
                         int argc, char **argv,
                         noit_console_state_t *state, void *closure) {
  noit_hash_iter iter = NOIT_HASH_ITER_ZERO;
  const char *k;
  int klen;
  void *data;
  int i, cnt, titled = 0, cliplen = 0;
  const char *path = "", *basepath = NULL;
  char xpath[1024];
  noit_conf_t_userdata_t *info = NULL;
  noit_hash_table *config;
  xmlXPathObjectPtr pobj = NULL;
  xmlXPathContextPtr xpath_ctxt = NULL, current_ctxt;
  xmlDocPtr master_config = NULL;
  xmlNodePtr node = NULL;

  noit_conf_xml_xpath(&master_config, &xpath_ctxt);
  if(argc > 1) {
    nc_printf(ncct, "too many arguments\n");
    return -1;
  }

  info = noit_console_userdata_get(ncct, NOIT_CONF_T_USERDATA);
  if(info) path = basepath = info->path;
  if(!info && argc == 0) {
    nc_printf(ncct, "argument required when not in configuration mode\n");
    return -1;
  }

  if(argc == 1) path = argv[0];
  if(!basepath) basepath = path;

  /* { / } is a special case */
  if(!strcmp(basepath, "/")) basepath = "";
  if(!strcmp(path, "/")) path = "";

  if(!master_config) {
    nc_printf(ncct, "no config\n");
    return -1;
  }

  /* { / } is the only path that will end with a /
   * in XPath { / / * } means something _entirely different than { / * }
   * Ever notice how it is hard to describe xpath in C comments?
   */
  /* We don't want to show the root node */
  cliplen = strlen("/noit/");

  /* If we are in configuration mode
   * and we are without an argument or the argument is absolute,
   * clip the current path off */
  if(info && (argc == 0 || path[0] != '/')) cliplen += strlen(basepath);
  if(!path[0] || path[0] == '/') /* base only, or absolute path requested */
    snprintf(xpath, sizeof(xpath), "/noit%s/@*", path);
  else
    snprintf(xpath, sizeof(xpath), "/noit%s/%s/@*", basepath, path);

  current_ctxt = xpath_ctxt;
  pobj = xmlXPathEval((xmlChar *)xpath, current_ctxt);
  if(!pobj || pobj->type != XPATH_NODESET) {
    nc_printf(ncct, "no such object\n");
    goto bad;
  }
  cnt = xmlXPathNodeSetGetLength(pobj->nodesetval);
  titled = 0;
  for(i=0; i<cnt; i++) {
    node = (noit_conf_section_t)xmlXPathNodeSetItem(pobj->nodesetval, i);
    if(!strcmp((char *)node->name, "check")) continue;
    if(node->children && node->children == xmlGetLastChild(node) &&
      xmlNodeIsText(node->children)) {
      if(!titled++) nc_printf(ncct, "== Section Settings ==\n");
      nc_printf(ncct, "%s: %s\n", xmlGetNodePath(node) + cliplen,
                xmlXPathCastNodeToString(node->children));
    }
  }
  xmlXPathFreeObject(pobj);

  /* Print out all the config settings */
  if(!path[0] || path[0] == '/') /* base only, or absolute path requested */
    snprintf(xpath, sizeof(xpath), "/noit%s", path);
  else
    snprintf(xpath, sizeof(xpath), "/noit%s/%s", basepath, path);
  pobj = xmlXPathEval((xmlChar *)xpath, current_ctxt);
  if(!pobj || pobj->type != XPATH_NODESET) {
    nc_printf(ncct, "no such object\n");
    goto bad;
  }
  cnt = xmlXPathNodeSetGetLength(pobj->nodesetval);
  node = (noit_conf_section_t)xmlXPathNodeSetItem(pobj->nodesetval, 0);
  titled = 0;
  config = noit_conf_get_hash(node, "config");
  while(noit_hash_next(config, &iter, &k, &klen, &data)) {
    if(!titled++) nc_printf(ncct, "== Section [Aggregated] Config ==\n");
    nc_printf(ncct, "config::%s: %s\n", k, (const char *)data);
  }
  noit_hash_destroy(config, free, free);
  free(config);
  xmlXPathFreeObject(pobj);

  /* _shorten string_ turning last { / @ * } to { / * } */
  if(!path[0] || path[0] == '/') /* base only, or absolute path requested */
    snprintf(xpath, sizeof(xpath), "/noit%s/*", path);
  else
    snprintf(xpath, sizeof(xpath), "/noit%s/%s/*", basepath, path);
  pobj = xmlXPathEval((xmlChar *)xpath, current_ctxt);
  if(!pobj || pobj->type != XPATH_NODESET) {
    nc_printf(ncct, "no such object\n");
    goto bad;
  }
  cnt = xmlXPathNodeSetGetLength(pobj->nodesetval);
  titled = 0;
  for(i=0; i<cnt; i++) {
    node = (noit_conf_section_t)xmlXPathNodeSetItem(pobj->nodesetval, i);
    if(!strcmp((char *)node->name, "check")) continue;
    if(!strcmp((char *)xmlGetNodePath(node) + cliplen, "config")) continue;
    if(!(node->children && node->children == xmlGetLastChild(node) &&
         xmlNodeIsText(node->children))) {
      if(!titled++) nc_printf(ncct, "== Subsections ==\n");
      nc_printf(ncct, "%s\n", xmlGetNodePath(node) + cliplen);
    }
  }

  titled = 0;
  for(i=0; i<cnt; i++) {
    node = (noit_conf_section_t)xmlXPathNodeSetItem(pobj->nodesetval, i);
    if(!strcmp((char *)node->name, "check")) {
      int busted = 1;
      xmlAttr *attr;
      char *uuid_str = "undefined";

      if(!titled++) nc_printf(ncct, "== Checks ==\n");

      for(attr=node->properties; attr; attr = attr->next) {
        if(!strcmp((char *)attr->name, "uuid"))
          uuid_str = (char *)xmlXPathCastNodeToString(attr->children);
      }
      if(uuid_str) {
        uuid_t checkid;
        nc_printf(ncct, "check[@uuid=\"%s\"] ", uuid_str);
        if(uuid_parse(uuid_str, checkid) == 0) {
          noit_check_t *check;
          check = noit_poller_lookup(checkid);
          if(check) {
            busted = 0;
            nc_printf(ncct, "%s`%s`%s", check->target, check->module, check->name);
          }
        }
      }
      else
        nc_printf(ncct, "%s ", xmlGetNodePath(node) + cliplen);
      if(busted) nc_printf(ncct, "[check not in running system]");
      nc_write(ncct, "\n", 1);
    }
  }
  xmlXPathFreeObject(pobj);
  return 0;
 bad:
  if(pobj) xmlXPathFreeObject(pobj);
  return -1;
}

static char *
conf_t_check_prompt(EditLine *el) {
  noit_console_closure_t ncct;
  noit_conf_t_userdata_t *info;
  noit_check_t *check;
  static char *tl = "noit(conf)# ";
  static char *pfmt = "noit(conf:%s%s%s)# ";

  el_get(el, EL_USERDATA, (void *)&ncct);
  if(!ncct) return tl;
  info = noit_console_userdata_get(ncct, NOIT_CONF_T_USERDATA);
  if(!info) return tl;

  check = noit_poller_lookup(info->current_check);
  if(check &&
     check->target && check->target[0] &&
     check->name && check->name[0])
    snprintf(info->prompt, sizeof(info->prompt),
             pfmt, check->target, "`", check->name);
  else {
    char uuid_str[37];
    uuid_unparse_lower(info->current_check, uuid_str);
    snprintf(info->prompt, sizeof(info->prompt), pfmt, "[", uuid_str, "]");
  }
  return info->prompt;
}
static char *
conf_t_prompt(EditLine *el) {
  noit_console_closure_t ncct;
  noit_conf_t_userdata_t *info;
  static char *tl = "noit(conf)# ";
  static char *pfmt = "noit(conf:%s%s)# ";
  int path_len, max_len;

  el_get(el, EL_USERDATA, (void *)&ncct);
  if(!ncct) return tl;
  info = noit_console_userdata_get(ncct, NOIT_CONF_T_USERDATA);
  if(!info) return tl;

  path_len = strlen(info->path);
  max_len = sizeof(info->prompt) - (strlen(pfmt) - 4 /* %s%s */) - 1 /* \0 */;
  if(path_len > max_len)
    snprintf(info->prompt, sizeof(info->prompt),
             pfmt, "...", info->path + path_len - max_len + 3 /* ... */);
  else
    snprintf(info->prompt, sizeof(info->prompt), pfmt, "", info->path);
  return info->prompt;
}
static int
noit_conf_checks_reload(noit_console_closure_t ncct,
                        int argc, char **argv,
                        noit_console_state_t *state, void *closure) {
  if(noit_conf_reload(ncct, argc, argv, state, closure)) return -1;
  noit_poller_reload(NULL);
  return 0;
}

static int
validate_attr_set_scope(noit_conf_t_userdata_t *info,
                        struct _valid_attr_t *attrinfo) {
  int len;
  len = strlen(attrinfo->scope);
  if(strncmp(info->path, attrinfo->scope, len) ||
     (info->path[len] != '\0' && info->path[len] != '/')) {
    return -1;
  }
  return 0;
}
static int
replace_config(noit_console_closure_t ncct,
               noit_conf_t_userdata_t *info, const char *name,
               const char *value) {
  int i, cnt, rv = -1, active = 0;
  xmlXPathObjectPtr pobj = NULL;
  xmlXPathContextPtr xpath_ctxt = NULL;
  xmlNodePtr node, confignode;
  char xpath[1024], *path;

  path = info->path;
  if(!strcmp(path, "/")) path = "";

  noit_conf_xml_xpath(NULL, &xpath_ctxt);

  /* Only if checks will fixate this attribute shall we check for
   * child <check> nodes.
   * NOTE: this return nothing and "seems" okay if we are _in_
   *       a <check> node.  That case is handled below.
   */
  snprintf(xpath, sizeof(xpath), "/noit/%s//check[@uuid]", path);
  pobj = xmlXPathEval((xmlChar *)xpath, xpath_ctxt);
  if(!pobj || pobj->type != XPATH_NODESET) goto out;
  cnt = xmlXPathNodeSetGetLength(pobj->nodesetval);
  for(i=0; i<cnt; i++) {
    uuid_t checkid;
    node = (noit_conf_section_t)xmlXPathNodeSetItem(pobj->nodesetval, i);
    if(noit_conf_get_uuid(node, "@uuid", checkid)) {
      noit_check_t *check;
      check = noit_poller_lookup(checkid);
      if(check && NOIT_CHECK_LIVE(check)) active++;
    }
  }
  if(pobj) xmlXPathFreeObject(pobj);

  snprintf(xpath, sizeof(xpath), "/noit/%s", path);
  pobj = xmlXPathEval((xmlChar *)xpath, xpath_ctxt);
  if(!pobj || pobj->type != XPATH_NODESET) goto out;
  cnt = xmlXPathNodeSetGetLength(pobj->nodesetval);
  if(cnt != 1) {
    nc_printf(ncct, "Internal error: context node disappeared\n");
    goto out;
  }
  node = (noit_conf_section_t)xmlXPathNodeSetItem(pobj->nodesetval, 0);
  if(strcmp((const char *)node->name, "check")) {
    uuid_t checkid;
    /* Detect if  we are actually a <check> node and attempting to
     * change something we shouldn't.
     * This is the counterpart noted above.
     */
    if(noit_conf_get_uuid(node, "@uuid", checkid)) {
      noit_check_t *check;
      check = noit_poller_lookup(checkid);
      if(NOIT_CHECK_LIVE(check)) active++;
    }
  }
#ifdef UNSAFE_RECONFIG
  if(active) {
    nc_printf(ncct, "Cannot set '%s', it would effect %d live check(s)\n",
              name, active);
    goto out;
  }
#endif
  if(pobj) xmlXPathFreeObject(pobj);

  /* Here we want to remove /noit/path/config/name */
  snprintf(xpath, sizeof(xpath), "/noit/%s/config/%s", path, name);
  pobj = xmlXPathEval((xmlChar *)xpath, xpath_ctxt);
  if(!pobj || pobj->type != XPATH_NODESET) goto out;
  if(xmlXPathNodeSetGetLength(pobj->nodesetval) > 0) {
    xmlNodePtr toremove;
    toremove = xmlXPathNodeSetItem(pobj->nodesetval, 0);
    xmlUnlinkNode(toremove);
  }
  /* TODO: if there are no more children of config, remove config? */
  if(value) {
    if(pobj) xmlXPathFreeObject(pobj);
    /* He we create config if needed and place a child node under it */
    snprintf(xpath, sizeof(xpath), "/noit/%s/config", path);
    pobj = xmlXPathEval((xmlChar *)xpath, xpath_ctxt);
    if(!pobj || pobj->type != XPATH_NODESET) goto out;
    if(xmlXPathNodeSetGetLength(pobj->nodesetval) == 0) {
      if(pobj) xmlXPathFreeObject(pobj);
      snprintf(xpath, sizeof(xpath), "/noit/%s", path);
      pobj = xmlXPathEval((xmlChar *)xpath, xpath_ctxt);
      if(!pobj || pobj->type != XPATH_NODESET) goto out;
      if(xmlXPathNodeSetGetLength(pobj->nodesetval) != 1) {
        nc_printf(ncct, "Node disappeared from under you!\n");
        goto out;
      }
      confignode = xmlNewChild(xmlXPathNodeSetItem(pobj->nodesetval, 0),
                               NULL, (xmlChar *)"config", NULL);
      if(confignode == NULL) {
        nc_printf(ncct, "Error creating config child node.\n");
        goto out;
      }
    }
    else confignode = xmlXPathNodeSetItem(pobj->nodesetval, 0);

    assert(confignode);
    /* Now we create a child */
    xmlNewChild(confignode, NULL, (xmlChar *)name, (xmlChar *)value);
  }
  noit_conf_mark_changed();
  rv = 0;
 out:
  if(pobj) xmlXPathFreeObject(pobj);
  return rv;
}
static int
replace_attr(noit_console_closure_t ncct,
             noit_conf_t_userdata_t *info, struct _valid_attr_t *attrinfo,
             const char *value) {
  int i, cnt, rv = -1, active = 0;
  xmlXPathObjectPtr pobj = NULL;
  xmlXPathContextPtr xpath_ctxt = NULL;
  xmlNodePtr node;
  char xpath[1024], *path;

  path = info->path;
  if(!strcmp(path, "/")) path = "";

  noit_conf_xml_xpath(NULL, &xpath_ctxt);
  if(attrinfo->checks_fixate) {
    /* Only if checks will fixate this attribute shall we check for
     * child <check> nodes.
     * NOTE: this return nothing and "seems" okay if we are _in_
     *       a <check> node.  That case is handled below.
     */
    snprintf(xpath, sizeof(xpath), "/noit/%s//check[@uuid]", path);
    pobj = xmlXPathEval((xmlChar *)xpath, xpath_ctxt);
    if(!pobj || pobj->type != XPATH_NODESET) goto out;
    cnt = xmlXPathNodeSetGetLength(pobj->nodesetval);
    for(i=0; i<cnt; i++) {
      uuid_t checkid;
      node = (noit_conf_section_t)xmlXPathNodeSetItem(pobj->nodesetval, i);
      if(noit_conf_get_uuid(node, "@uuid", checkid)) {
        noit_check_t *check;
        check = noit_poller_lookup(checkid);
        if(check && NOIT_CHECK_LIVE(check)) active++;
      }
    }
    if(pobj) xmlXPathFreeObject(pobj);
  }
  snprintf(xpath, sizeof(xpath), "/noit/%s", path);
  pobj = xmlXPathEval((xmlChar *)xpath, xpath_ctxt);
  if(!pobj || pobj->type != XPATH_NODESET) goto out;
  cnt = xmlXPathNodeSetGetLength(pobj->nodesetval);
  if(cnt != 1) {
    nc_printf(ncct, "Internal error: context node disappeared\n");
    goto out;
  }
  node = (noit_conf_section_t)xmlXPathNodeSetItem(pobj->nodesetval, 0);
  if(attrinfo->checks_fixate &&
     !strcmp((const char *)node->name, "check")) {
    uuid_t checkid;
    /* Detect if  we are actually a <check> node and attempting to
     * change something we shouldn't.
     * This is the counterpart noted above.
     */
    if(noit_conf_get_uuid(node, "@uuid", checkid)) {
      noit_check_t *check;
      check = noit_poller_lookup(checkid);
      if(check && NOIT_CHECK_LIVE(check)) active++;
    }
  }
  if(active) {
    nc_printf(ncct, "Cannot set '%s', it would effect %d live check(s)\n",
              attrinfo->name, active);
    goto out;
  }
  xmlUnsetProp(node, (xmlChar *)attrinfo->name);
  if(value)
    xmlSetProp(node, (xmlChar *)attrinfo->name, (xmlChar *)value);
  noit_conf_mark_changed();
  rv = 0;
 out:
  if(pobj) xmlXPathFreeObject(pobj);
  return rv;
}
int
noit_conf_check_set_attr(noit_console_closure_t ncct,
                         int argc, char **argv,
                         noit_console_state_t *state, void *closure) {
  struct _valid_attr_t *attrinfo = closure;
  noit_conf_t_userdata_t *info;

  info = noit_console_userdata_get(ncct, NOIT_CONF_T_USERDATA);
  if(!info || validate_attr_set_scope(info, attrinfo)) {
    nc_printf(ncct, "'%s' attribute only valid in %s scope\n",
              attrinfo->name, attrinfo->scope);
    return -1;
  }

  if(argc != 1) {
    nc_printf(ncct, "set requires exactly one value\n");
    return -1;
  }
  /* Okay, we have an attribute and it should be set/replaced on the
   * current path.
   */
  if(replace_attr(ncct, info, attrinfo, argv[0])) {
    return -1;
  }

  /* So, we updated an attribute, so we need to reload all checks
   * that are descendent-or-self of this node.
   */
  if(!strncmp(info->path, "/checks", strlen("/checks")))
    refresh_subchecks(ncct, info);
  if(!strncmp(info->path, "/filtersets", strlen("/filtersets")))
    noit_refresh_filtersets(ncct, info);
  return 0;
}

int
noit_conf_check_unset_attr(noit_console_closure_t ncct,
                           int argc, char **argv,
                           noit_console_state_t *state, void *closure) {
  struct _valid_attr_t *attrinfo = closure;
  noit_conf_t_userdata_t *info;

  info = noit_console_userdata_get(ncct, NOIT_CONF_T_USERDATA);
  if(!info || validate_attr_set_scope(info, attrinfo)) {
    nc_printf(ncct, "'%s' attribute only valid in %s scope\n",
              attrinfo->name, attrinfo->scope);
    return -1;
  }

  if(argc != 0) {
    nc_printf(ncct, "no arguments allowed to this command.\n");
    return -1;
  }
  /* Okay, we have an attribute and it should be set/replaced on the
   * current path.
   */
  if(replace_attr(ncct, info, attrinfo, NULL)) {
    return -1;
  }

  /* So, we updated an attribute, so we need to reload all checks
   * that are descendent-or-self of this node.
   */
  if(!strncmp(info->path, "/checks", strlen("/checks")))
    refresh_subchecks(ncct, info);
  if(!strncmp(info->path, "/filterset", strlen("/filterest")))
    noit_refresh_filtersets(ncct, info);
  return 0;
}

int
noit_console_config_setconfig(noit_console_closure_t ncct,
                                int argc, char **argv,
                                noit_console_state_t *state, void *closure) {
  noit_conf_t_userdata_t *info;

  info = noit_console_userdata_get(ncct, NOIT_CONF_T_USERDATA);

  if(argc != 2) {
    nc_printf(ncct, "two arguments required.\n");
    return -1;
  }
  /* Okay, we have an child name and it should be culled from
   * current path/config.
   */
  if(replace_config(ncct, info, argv[0], argv[1])) {
    return -1;
  }

  /* So, we updated an attribute, so we need to reload all checks
   * that are descendent-or-self of this node.
   */
  refresh_subchecks(ncct, info);
  return 0;
}

int
noit_console_config_unsetconfig(noit_console_closure_t ncct,
                                int argc, char **argv,
                                noit_console_state_t *state, void *closure) {
  noit_conf_t_userdata_t *info;

  info = noit_console_userdata_get(ncct, NOIT_CONF_T_USERDATA);

  if(argc != 1) {
    nc_printf(ncct, "one argument required.\n");
    return -1;
  }
  /* Okay, we have an child name and it should be culled from
   * current path/config.
   */
  if(replace_config(ncct, info, argv[0], NULL)) {
    return -1;
  }

  /* So, we updated an attribute, so we need to reload all checks
   * that are descendent-or-self of this node.
   */
  refresh_subchecks(ncct, info);
  return 0;
}


#define NEW_STATE(a) (a) = noit_console_state_alloc()
#define ADD_CMD(a,cmd,func,ac,ss,c) \
  noit_console_state_add_cmd((a), \
    NCSCMD(cmd, func, ac, ss, c))
#define DELEGATE_CMD(a,cmd,ac,ss) \
  noit_console_state_add_cmd((a), \
    NCSCMD(cmd, noit_console_state_delegate, ac, ss, NULL))

static
void register_console_config_commands() {
  cmd_info_t *showcmd, *nocmd;
  noit_console_state_t *tl, *_conf_state, *_conf_t_state,
                       *_conf_t_check_state,
                       *_write_state, *_attr_state,
                       *_unset_state, *_uattr_state;

  tl = noit_console_state_initial();

  /* write <terimal|memory|file> */
  NEW_STATE(_write_state);
  ADD_CMD(_write_state, "terminal", noit_conf_write_terminal, NULL, NULL, NULL);
  ADD_CMD(_write_state, "file", noit_conf_write_file, NULL, NULL, NULL);
  /* write memory?  It's to a file, but I like router syntax */
  ADD_CMD(_write_state, "memory", noit_conf_write_file, NULL, NULL, NULL);

  /* attribute <attrname> <value> */
  NEW_STATE(_attr_state);
  noit_console_state_add_check_attrs(_attr_state, noit_conf_check_set_attr);
 
  /* no attribute <attrname> <value> */
  NEW_STATE(_uattr_state);
  noit_console_state_add_check_attrs(_uattr_state, noit_conf_check_unset_attr);

  NEW_STATE(_unset_state);
  DELEGATE_CMD(_unset_state, "attribute",
               noit_console_opt_delegate, _uattr_state);
  ADD_CMD(_unset_state, "section",
          noit_console_config_section, NULL, NULL, (void *)1);
  ADD_CMD(_unset_state, "config",
          noit_console_config_unsetconfig, NULL, NULL, NULL);
  ADD_CMD(_unset_state, "check",
          noit_console_config_nocheck, NULL, NULL, NULL);
 
  NEW_STATE(_conf_t_check_state);
  _conf_t_check_state->console_prompt_function = conf_t_check_prompt;
  DELEGATE_CMD(_conf_t_check_state, "attribute",
               noit_console_opt_delegate, _attr_state);
  DELEGATE_CMD(_conf_t_check_state, "no",
               noit_console_opt_delegate, _unset_state);
  ADD_CMD(_conf_t_check_state, "config",
          noit_console_config_setconfig, NULL, NULL, NULL);
  ADD_CMD(_conf_t_check_state, "status",
          noit_console_show_check, NULL, NULL, NULL);
  ADD_CMD(_conf_t_check_state, "exit",
          noit_console_config_cd, NULL, NULL, "..");
  ADD_CMD(_conf_t_check_state, "check",
          noit_console_check, noit_console_conf_check_opts,
          _conf_t_check_state, "..");

  NEW_STATE(_conf_t_state); 
  _conf_t_state->console_prompt_function = conf_t_prompt;
  noit_console_state_add_cmd(_conf_t_state, &console_command_exit);
  ADD_CMD(_conf_t_state, "ls", noit_console_config_show, NULL, NULL, NULL);
  ADD_CMD(_conf_t_state, "cd", noit_console_config_cd, NULL, NULL, NULL);
  ADD_CMD(_conf_t_state, "config",
          noit_console_config_setconfig, NULL, NULL, NULL);
  ADD_CMD(_conf_t_state, "section",
          noit_console_config_section, NULL, NULL, (void *)0);
  ADD_CMD(_conf_t_state, "check",
          noit_console_check, noit_console_conf_check_opts,
          _conf_t_check_state, NULL);

  showcmd = noit_console_state_get_cmd(tl, "show");
  ADD_CMD(showcmd->dstate, "check",
          noit_console_show_check, noit_console_check_opts, NULL, NULL);

  ADD_CMD(tl, "watch",
          noit_console_watch_check, noit_console_check_opts, NULL, (void *)1);

  nocmd = noit_console_state_get_cmd(tl, "no");
  ADD_CMD(nocmd->dstate, "watch",
          noit_console_watch_check, noit_console_check_opts, NULL, (void *)0);

  DELEGATE_CMD(_conf_t_state, "write",
               noit_console_opt_delegate, _write_state);
  DELEGATE_CMD(_conf_t_state, "attribute",
               noit_console_opt_delegate, _attr_state);
  DELEGATE_CMD(_conf_t_state, "no", noit_console_opt_delegate, _unset_state);

  NEW_STATE(_conf_state);
  ADD_CMD(_conf_state, "terminal",
          noit_console_state_conf_terminal, NULL, _conf_t_state, NULL);

  ADD_CMD(tl, "configure",
          noit_console_state_delegate, noit_console_opt_delegate,
          _conf_state, NULL);
  ADD_CMD(tl, "write",
          noit_console_state_delegate, noit_console_opt_delegate,
          _write_state, NULL);
  ADD_CMD(tl, "reload", noit_conf_checks_reload, NULL, NULL, NULL);
}

