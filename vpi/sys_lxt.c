/*
 * Copyright (c) 2002 Stephen Williams (steve@icarus.com)
 *
 *    This source code is free software; you can redistribute it
 *    and/or modify it in source code form under the terms of the GNU
 *    General Public License as published by the Free Software
 *    Foundation; either version 2 of the License, or (at your option)
 *    any later version.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with this program; if not, write to the Free Software
 *    Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA
 */
#if !defined(WINNT) && !defined(macintosh)
#ident "$Id: sys_lxt.c,v 1.9 2002/07/17 05:13:43 steve Exp $"
#endif

# include "config.h"
# include "lxt_write.h"

/*
 * This file contains the implementations of the VCD related
 * funcitons.
 */

# include  "vpi_user.h"
# include  <stdio.h>
# include  <stdlib.h>
# include  <string.h>
# include  <assert.h>
# include  <time.h>
#ifdef HAVE_MALLOC_H
# include  <malloc.h>
#endif

/*
 * The lxt_scope head and current pointers are used to keep a scope
 * stack that can be accessed from the bottom. The lxt_scope_head
 * points to the first (bottom) item in the stack and
 * lxt_scope_current points to the last (top) item in the stack. The
 * push_scope and pop_scope methods manipulate the stack.
 */
struct lxt_scope
{
      struct lxt_scope *next, *prev;
      char *name;
      int len;
};

static struct lxt_scope *lxt_scope_head=NULL,  *lxt_scope_current=NULL;

static void push_scope(const char *name)
{
      struct lxt_scope *t = (struct lxt_scope *)
	    calloc(1, sizeof(struct lxt_scope));

      t->name = strdup(name);
      t->len = strlen(name);

      if(!lxt_scope_head) {
	    lxt_scope_head = lxt_scope_current = t;
      } else {
	    lxt_scope_current->next = t;
	    t->prev = lxt_scope_current;
	    lxt_scope_current = t;
      }
}

static void pop_scope(void)
{
      struct lxt_scope *t;

      assert(lxt_scope_current);

      t=lxt_scope_current->prev;
      free(lxt_scope_current->name);
      free(lxt_scope_current);
      lxt_scope_current = t;
      if (lxt_scope_current) {
	    lxt_scope_current->next = 0;
      } else {
	    lxt_scope_head = 0;
      }
}

/*
 * This function uses the scope stack to generate a hierarchical
 * name. Scan the scope stack from the bottom up to construct the
 * name.
 */
static char *create_full_name(const char *name)
{
      char *n, *n2;
      int len = 0;
      struct lxt_scope *t = lxt_scope_head;

	/* Figure out how long the combined string will be. */
      while(t) {
	    len+=t->len+1;
	    t=t->next;
      }

      len += strlen(name) + 1;

	/* Allocate a string buffer. */
      n = n2 = malloc(len);

      t = lxt_scope_head;
      while(t) {
	    strcpy(n2, t->name);
	    n2 += t->len;
	    *n2 = '.';
	    n2++;
	    t=t->next;
      }

      strcpy(n2, name);
      n2 += strlen(n2);
      assert( (n2 - n + 1) == len );

      return(n);
}


static struct lt_trace *dump_file = 0;

struct vcd_info {
      vpiHandle item;
      vpiHandle cb;
      struct t_vpi_time time;
      struct lt_symbol *sym;
      struct vcd_info*next;
};


static struct vcd_info*vcd_list = 0;
static unsigned long vcd_cur_time = 0;
static int dump_is_off = 0;


static void show_this_item(struct vcd_info*info)
{
      s_vpi_value value;

      value.format = vpiBinStrVal;
      vpi_get_value(info->item, &value);
      lt_emit_value_bit_string(dump_file, info->sym, 0 /* array row */, value.value.str);
}


static void show_this_item_x(struct vcd_info*info)
{
      lt_emit_value_bit_string(dump_file, info->sym, 0 /* array row */, "x");
}


/*
 * managed qsorted list of scope names for duplicates bsearching
 */

struct vcd_names_s {
      const char *name;
      struct vcd_names_s *next;
};

static struct vcd_names_s *vcd_names_list;
static const char **vcd_names_sorted;
static int listed_names, sorted_names;

inline static void vcd_names_add(const char *name)
{
      struct vcd_names_s *nl = (struct vcd_names_s *)
	    malloc(sizeof(struct vcd_names_s));
      assert(nl);
      nl->name = strdup(name);
      nl->next = vcd_names_list;
      vcd_names_list = nl;
      listed_names ++;
}

static int vcd_names_compare(const void *s1, const void *s2)
{
      const char *v1 = *(const char **) s1;
      const char *v2 = *(const char **) s2;

      return strcmp(v1, v2);
}

static const char *vcd_names_search(const char *key)
{
      const char **v = (const char **)
	    bsearch(&key, 
		    vcd_names_sorted, sorted_names,
		    sizeof(const char *), vcd_names_compare );
      
      return(v ? *v : NULL);
}

static void vcd_names_sort(void)
{
      if (listed_names) {
	    struct vcd_names_s *r; 
	    const char **l;
	    
	    sorted_names += listed_names;
	    vcd_names_sorted = (const char **) 
		  realloc(vcd_names_sorted, 
			  sorted_names*(sizeof(const char *)));
	    assert(vcd_names_sorted);
	    
	    l = vcd_names_sorted + sorted_names - listed_names;
	    listed_names = 0;

	    r = vcd_names_list;
	    vcd_names_list = 0x0;

	    while (r) {
		  struct vcd_names_s *rr = r;
		  r = rr->next;
		  *(l++) = rr->name;
		  free(rr);
	    }
	    
	    qsort(vcd_names_sorted, sorted_names, 
		  sizeof(const char **), vcd_names_compare);
      }
}


static int dumpvars_status = 0; /* 0:fresh 1:cb installed, 2:callback done */
static unsigned long dumpvars_time;
inline static int dump_header_pending(void)
{
      return dumpvars_status != 2;
}

/*
 * This function writes out all the traced variables, whether they
 * changed or not.
 */
static void vcd_checkpoint()
{
      struct vcd_info*cur;

      for (cur = vcd_list ;  cur ;  cur = cur->next)
	    show_this_item(cur);
}

static void vcd_checkpoint_x()
{
      struct vcd_info*cur;

      for (cur = vcd_list ;  cur ;  cur = cur->next)
	    show_this_item_x(cur);
}

static int variable_cb(p_cb_data cause)
{
      unsigned long now = cause->time->low;
      struct vcd_info*info = (struct vcd_info*)cause->user_data;

      if (dump_is_off)
	    return 0;

      if (dump_header_pending())
	    return 0;

      if (now != vcd_cur_time) {
            lt_set_time(dump_file, now);
	    vcd_cur_time = now;
      }

      show_this_item(info);

      return 0;
}

static int dumpvars_cb(p_cb_data cause)
{
      if (dumpvars_status != 1)
	    return 0;

      dumpvars_status = 2;

      dumpvars_time = cause->time->low;
      vcd_cur_time = dumpvars_time;

      if (!dump_is_off) {
            lt_set_time(dump_file, dumpvars_time);
	    vcd_checkpoint();
      }

      return 0;
}

inline static int install_dumpvars_callback(void)
{
      struct t_cb_data cb;
      static struct t_vpi_time time;

      if (dumpvars_status == 1)
	    return 0;

      if (dumpvars_status == 2) {
	    vpi_mcd_printf(6, "VCD Error:"
			   " $dumpvars ignored,"
			   " previously called at simtime %lu\n",
			   dumpvars_time);
	    return 1;
      }

      time.type = vpiSimTime;
      cb.time = &time;
      cb.reason = cbReadOnlySynch;
      cb.cb_rtn = dumpvars_cb;
      cb.user_data = 0x0;
      cb.obj = 0x0;

      vpi_register_cb(&cb);

      dumpvars_status = 1;
      return 0;
}

static int sys_dumpoff_calltf(char*name)
{
      s_vpi_time now;

      if (dump_is_off)
	    return 0;

      dump_is_off = 1;

      if (dump_file == 0)
	    return 0;

      if (dump_header_pending())
	    return 0;

      vpi_get_time(0, &now);
      if (now.low > vcd_cur_time)
            lt_set_time(dump_file, now.low);
      vcd_cur_time = now.low;

      vcd_checkpoint_x();

      return 0;
}

static int sys_dumpon_calltf(char*name)
{
      s_vpi_time now;

      if (!dump_is_off)
	    return 0;

      dump_is_off = 0;

      if (dump_file == 0)
	    return 0;

      if (dump_header_pending())
	    return 0;

      vpi_get_time(0, &now);
      if (now.low > vcd_cur_time)
            lt_set_time(dump_file, now.low);
      vcd_cur_time = now.low;

      vcd_checkpoint();

      return 0;
}

static int sys_dumpall_calltf(char*name)
{
      s_vpi_time now;

      if (dump_file == 0)
	    return 0;

      if (dump_header_pending())
	    return 0;

      vpi_get_time(0, &now);
      if (now.low > vcd_cur_time)
            lt_set_time(dump_file, now.low);
      vcd_cur_time = now.low;

      vcd_checkpoint();

      return 0;
}

static void *close_dumpfile(void)
{
lt_close(dump_file);
return(dump_file = NULL);
}

static void open_dumpfile(const char*path)
{
      dump_file = lt_init(path);

      if (dump_file == 0) {
	    vpi_mcd_printf(6, 
			   "LXT Error: Unable to open %s for output.\n", 
			   path);
	    return;
      } else {
	    int prec = vpi_get(vpiTimePrecision, 0);

	    vpi_mcd_printf(4, 
			   "LXT info: dumpfile %s opened for output.\n", 
			   path);
	    
	    assert(prec >= -15);
	    lt_set_timescale(dump_file, prec);

	    lt_set_initial_value(dump_file, 'x');
            lt_set_clock_compress(dump_file);

            atexit((void(*)(void))close_dumpfile);
      }
}

static int sys_dumpfile_calltf(char*name)
{
      char*path;

      vpiHandle sys = vpi_handle(vpiSysTfCall, 0);
      vpiHandle argv = vpi_iterate(vpiArgument, sys);
      vpiHandle item;

      if (argv && (item = vpi_scan(argv))) {
	    s_vpi_value value;

	    if (vpi_get(vpiType, item) != vpiConstant
		|| vpi_get(vpiConstType, item) != vpiStringConst) {
		  vpi_mcd_printf(6, 
				 "LXT Error:"
				 " %s parameter must be a string constant\n", 
				 name);
		  return 0;
	    }

	    value.format = vpiStringVal;
	    vpi_get_value(item, &value);
	    path = strdup(value.value.str);

	    vpi_free_object(argv);

      } else {
	    path = strdup("dumpfile.lxt");
      }

      assert(dump_file == 0);
      open_dumpfile(path);

      free(path);

      return 0;
}

/* 
   Nexus Id cache

   In structural models, many signals refer to the same nexus.
   Some structural models also have very many signals.  This cache
   saves nexus_id - vcd_id pairs, and reuses the vcd_id when a signal 
   refers to a nexus that is already dumped.

   The new signal will be listed as a $var, but no callback 
   will be installed.  This saves considerable CPU time and leads 
   to smalle VCD files.

   The _vpiNexusId is a private (int) property of IVL simulators.
*/

struct vcd_id_s 
{
  const char *id;
  struct vcd_id_s *next;
  int nex;
};

static inline unsigned ihash(int nex)
{
  unsigned a = nex;
  a ^= a>>16;
  a ^= a>>8;
  return a & 0xff;
}

static struct vcd_id_s **vcd_ids;

inline static const char *find_nexus_ident(int nex)
{
      struct vcd_id_s *bucket;
      
      if (!vcd_ids) {
	    vcd_ids = (struct vcd_id_s **)
		  calloc(256, sizeof(struct vcd_id_s*));
	    assert(vcd_ids);
      }

      bucket = vcd_ids[ihash(nex)];
      while (bucket) {
	    if (nex == bucket->nex)
		  return bucket->id;
	    bucket = bucket->next;
      }

      return 0;
}

inline static void set_nexus_ident(int nex, const char *id)
{
      struct vcd_id_s *bucket;

      assert(vcd_ids);

      bucket = (struct vcd_id_s *) malloc(sizeof(struct vcd_id_s));
      bucket->next = vcd_ids[ihash(nex)];
      bucket->id = id;
      bucket->nex = nex;
      vcd_ids[ihash(nex)] = bucket;
}

static void scan_item(unsigned depth, vpiHandle item, int skip)
{
      struct t_cb_data cb;
      struct vcd_info* info;

      const char* type;
      const char* name;
      const char* ident;
      int nexus_id;

      /* list of types to iterate upon */
      int i;
      static int types[] = {
	    /* Value */
	    vpiNet,
	    vpiReg,
	    vpiVariables,
	    /* Scope */
	    vpiFunction,
	    vpiModule,
	    vpiNamedBegin,
	    vpiNamedFork,
	    vpiTask,
	    -1
      };

      switch (vpi_get(vpiType, item)) {

	  case vpiMemory:
	      /* don't know how to watch memories. */
	    break;

	  case vpiNamedEvent:
	      /* There is nothing in named events to dump. */
	    break;

	  case vpiNet:  type = "wire";    if(0){
	  case vpiIntegerVar:
	  case vpiTimeVar:
	  case vpiReg:  type = "reg";    }

	    if (skip)
		  break;
	    
	    name = vpi_get_str(vpiName, item);
	    nexus_id = vpi_get(_vpiNexusId, item);
	    if (nexus_id) {
		  ident = find_nexus_ident(nexus_id);
	    } else {
		  ident = 0;
	    }
	    
	    if (!ident) {
		  ident = create_full_name(name);
		  
		  if (nexus_id)
			set_nexus_ident(nexus_id, ident);
		  
		  info = malloc(sizeof(*info));

		  info->time.type = vpiSimTime;
		  info->item  = item;
		  info->sym   = lt_symbol_add(dump_file, ident, 0 /* array rows */, vpi_get(vpiSize, item)-1, 0, LT_SYM_F_BITS);

		  cb.time      = &info->time;
		  cb.user_data = (char*)info;
		  cb.value     = NULL;
		  cb.obj       = item;
		  cb.reason    = cbValueChange;
		  cb.cb_rtn    = variable_cb;

		  info->next  = vcd_list;
		  vcd_list    = info;

		  info->cb    = vpi_register_cb(&cb);

	    } else {
		  char *n = create_full_name(name);
		  lt_symbol_alias(dump_file, ident, n,
				  vpi_get(vpiSize, item)-1, 0);
		  free(n);
            }
	    
	    break;
	    
	  case vpiModule:      type = "module";      if(0){
	  case vpiNamedBegin:  type = "begin";      }if(0){
	  case vpiTask:        type = "task";       }if(0){
	  case vpiFunction:    type = "function";   }if(0){
	  case vpiNamedFork:   type = "fork";       }

	    if (depth > 0) {
		  int nskip;
		  vpiHandle argv;

		  const char* fullname =
			vpi_get_str(vpiFullName, item);

		  vpi_mcd_printf(4, 
				 "LXT info:"
				 " scanning scope %s, %u levels\n",
				 fullname, depth);

		  nskip = sorted_names && fullname
			&& vcd_names_search(fullname);
		  
		  if (!nskip) 
			vcd_names_add(fullname);
		  else 
		    vpi_mcd_printf(6,
				   "LXT warning:"
				   " ignoring signals"
				   " in previously scanned scope %s\n",
				   fullname);

		  name = vpi_get_str(vpiName, item);

                  push_scope(name);	/* keep in type info determination for possible future usage */
		  
		  for (i=0; types[i]>0; i++) {
			vpiHandle hand;
			argv = vpi_iterate(types[i], item);
			while (argv && (hand = vpi_scan(argv))) {
			      scan_item(depth-1, hand, nskip);
			}
		  }
		  
                  pop_scope();
	    }
	    break;
	    
	  default:
	    vpi_mcd_printf(6,
			   "LXT Error: $lxtdumpvars: Unsupported parameter "
			   "type (%d)\n", vpi_get(vpiType, item));
      }

}

static int draw_scope(vpiHandle item)
{
      int depth;
      const char *name;
      char *type;

      vpiHandle scope = vpi_handle(vpiScope, item);
      if (!scope)
	    return 0;
      
      depth = 1 + draw_scope(scope);
      name = vpi_get_str(vpiName, scope);

      switch (vpi_get(vpiType, item)) {
	  case vpiNamedBegin:  type = "begin";      break;
	  case vpiTask:        type = "task";       break;
	  case vpiFunction:    type = "function";   break;
	  case vpiNamedFork:   type = "fork";       break;
      	  default:             type = "module";     break;
      }
      
      push_scope(name);	/* keep in type info determination for possible future usage */

      return depth;
}

static int sys_dumpvars_calltf(char*name)
{
      unsigned depth;
      s_vpi_value value;
      vpiHandle item = 0;
      vpiHandle sys = vpi_handle(vpiSysTfCall, 0);
      vpiHandle argv;

      if (dump_file == 0) {
	    open_dumpfile("dumpfile.lxt");
	    if (dump_file == 0)
		  return 0;
      }

      if (install_dumpvars_callback()) {
	    return 0;
      }

      argv = vpi_iterate(vpiArgument, sys);

      depth = 0;
      if (argv && (item = vpi_scan(argv)))
	    switch (vpi_get(vpiType, item)) {
		case vpiConstant:
		case vpiNet:
		case vpiIntegerVar:
		case vpiReg:
		case vpiMemoryWord:
		  value.format = vpiIntVal;
		  vpi_get_value(item, &value);
		  depth = value.value.integer;
		  break;
	    }

      if (!depth)
	    depth = 10000;

      if (!argv) {
	    // $dumpvars;
	    // search for the toplevel module
	    vpiHandle parent = vpi_handle(vpiScope, sys);
	    while (parent) {
		  item = parent;
		  parent = vpi_handle(vpiScope, item);
	    }

      } else if (!item  ||  !(item = vpi_scan(argv))) {
	    // $dumpvars(level);
	    // $dumpvars();
	    // dump the current scope
	    item = vpi_handle(vpiScope, sys);
	    argv = 0x0;
      }

      for ( ; item; item = argv ? vpi_scan(argv) : 0x0) {

	    int dep = draw_scope(item);

	    vcd_names_sort();
	    scan_item(depth, item, 0);
	    
	    while (dep--) {
		  pop_scope();
	    }
      }

      return 0;
}

void sys_lxt_register()
{
      s_vpi_systf_data tf_data;

      tf_data.type      = vpiSysTask;
      tf_data.tfname    = "$dumpall";
      tf_data.calltf    = sys_dumpall_calltf;
      tf_data.compiletf = 0;
      tf_data.sizetf    = 0;
      tf_data.user_data = "$dumpall";
      vpi_register_systf(&tf_data);

      tf_data.type      = vpiSysTask;
      tf_data.tfname    = "$dumpoff";
      tf_data.calltf    = sys_dumpoff_calltf;
      tf_data.compiletf = 0;
      tf_data.sizetf    = 0;
      tf_data.user_data = "$dumpoff";
      vpi_register_systf(&tf_data);

      tf_data.type      = vpiSysTask;
      tf_data.tfname    = "$dumpon";
      tf_data.calltf    = sys_dumpon_calltf;
      tf_data.compiletf = 0;
      tf_data.sizetf    = 0;
      tf_data.user_data = "$dumpon";
      vpi_register_systf(&tf_data);

      tf_data.type      = vpiSysTask;
      tf_data.tfname    = "$dumpfile";
      tf_data.calltf    = sys_dumpfile_calltf;
      tf_data.compiletf = 0;
      tf_data.sizetf    = 0;
      tf_data.user_data = "$dumpfile";
      vpi_register_systf(&tf_data);

      tf_data.type      = vpiSysTask;
      tf_data.tfname    = "$dumpvars";
      tf_data.calltf    = sys_dumpvars_calltf;
      tf_data.compiletf = 0;
      tf_data.sizetf    = 0;
      tf_data.user_data = "$dumpvars";
      vpi_register_systf(&tf_data);
}

/*
 * $Log: sys_lxt.c,v $
 * Revision 1.9  2002/07/17 05:13:43  steve
 *  Implementation of vpi_handle_by_name, and
 *  add the vpiVariables iterator.
 *
 * Revision 1.8  2002/07/15 03:57:30  steve
 *  Fix dangling pointer in pop_scope.
 *
 * Revision 1.7  2002/07/12 17:09:21  steve
 *  Remember to scan IntegerVars.
 *
 * Revision 1.6  2002/07/12 17:08:13  steve
 *  Eliminate use of vpiInternalScope.
 *
 * Revision 1.5  2002/06/21 04:59:36  steve
 *  Carry integerness throughout the compilation.
 *
 * Revision 1.4  2002/06/03 03:56:06  steve
 *  Ignore memories and named events.
 *
 * Revision 1.3  2002/04/06 21:33:29  steve
 *  allow runtime selection of VCD vs LXT.
 *
 * Revision 1.2  2002/04/06 20:25:45  steve
 *  cbValueChange automatically replays.
 *
 * Revision 1.1  2002/03/09 21:54:49  steve
 *  Add LXT dumper support. (Anthony Bybell)
 *
 */

