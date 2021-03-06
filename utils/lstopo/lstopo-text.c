/*
 * Copyright © 2009 CNRS
 * Copyright © 2009-2016 Inria.  All rights reserved.
 * Copyright © 2009-2012 Université Bordeaux
 * Copyright © 2009-2011 Cisco Systems, Inc.  All rights reserved.
 * See COPYING in top-level directory.
 */

#include <private/autogen/config.h>
#include <private/private.h>
#include <private/misc.h>
#include <hwloc.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include "lstopo.h"
#include "misc.h"

#define indent(output, i) \
  fprintf (output, "%*s", (int) i, "");

/*
 * Console fashion text output
 */

static void
output_console_obj (struct lstopo_output *loutput, hwloc_obj_t l, int collapse)
{
  hwloc_topology_t topology = loutput->topology;
  FILE *output = loutput->file;
  int logical = loutput->logical;
  int verbose_mode = loutput->verbose_mode;
  unsigned idx = logical ? l->logical_index : l->os_index;
  char pidxstr[16];
  char lidxstr[16];
  char busidstr[32];

  if (collapse > 1 && l->type == HWLOC_OBJ_PCI_DEVICE) {
    strcpy(pidxstr, "P#[collapsed]"); /* shouldn't be used, os_index should be -1 except if importing old XMLs */
    snprintf(lidxstr, sizeof(lidxstr), "L#%u-%u", l->logical_index, l->logical_index+collapse-1);
  } else {
    snprintf(pidxstr, sizeof(pidxstr), "P#%u", l->os_index);
    snprintf(lidxstr, sizeof(lidxstr), "L#%u", l->logical_index);
  }
  if (l->type == HWLOC_OBJ_PCI_DEVICE)
    lstopo_busid_snprintf(busidstr, sizeof(busidstr), l, collapse, topology->pci_nonzero_domains);

  if (loutput->show_cpuset < 2) {
    char type[64], *attr, phys[32] = "";
    int len;
    hwloc_obj_type_snprintf (type, sizeof(type), l, verbose_mode-1);
    if (l->subtype)
      fprintf(output, "%s(%s)", type, l->subtype);
    else
      fprintf(output, "%s", type);
    if (l->depth != 0 && idx != (unsigned)-1
	&& (verbose_mode >= 2 || !hwloc_obj_type_is_special(l->type)))
      fprintf(output, " %s", logical ? lidxstr : pidxstr);
    if (l->name && (l->type == HWLOC_OBJ_MISC || l->type == HWLOC_OBJ_GROUP))
      fprintf(output, " %s", l->name);
    if (logical && l->os_index != (unsigned) -1 &&
	(verbose_mode >= 2 || l->type == HWLOC_OBJ_PU || l->type == HWLOC_OBJ_NUMANODE))
      snprintf(phys, sizeof(phys), "%s", pidxstr);
    if (l->type == HWLOC_OBJ_PCI_DEVICE && verbose_mode <= 1)
      fprintf(output, " %s (%s)",
	      busidstr, hwloc_pci_class_string(l->attr->pcidev.class_id));
    /* display attributes */
    len = hwloc_obj_attr_snprintf (NULL, 0, l, " ", verbose_mode-1);
    attr = malloc(len+1);
    *attr = '\0';
    hwloc_obj_attr_snprintf (attr, len+1, l, " ", verbose_mode-1);
    if (*phys || *attr) {
      fprintf(output, " (");
      if (*phys)
	fprintf(output, "%s", phys);
      if (*phys && *attr)
	fprintf(output, " ");
      if (*attr) {
	if (collapse > 1 && l->type == HWLOC_OBJ_PCI_DEVICE) {
	  assert(!strncmp(attr, "busid=", 6));
	  assert(!strncmp(attr+18, " id=", 4));
	  fprintf(output, "busid=%s%s", busidstr, attr+18);
	} else
	  fprintf(output, "%s", attr);
      }
      fprintf(output, ")");
    }
    free(attr);
    /* display the root total_memory if not verbose (already shown)
     * and different from the local_memory (already shown) */
    if (verbose_mode == 1 && !l->parent && l->memory.total_memory > l->memory.local_memory)
      fprintf(output, " (%lu%s total)",
	      (unsigned long) hwloc_memory_size_printf_value(l->memory.total_memory, 0),
	      hwloc_memory_size_printf_unit(l->memory.total_memory, 0));
    /* append the name */
    if (l->name && (l->type == HWLOC_OBJ_OS_DEVICE || verbose_mode >= 2)
	&& l->type != HWLOC_OBJ_MISC && l->type != HWLOC_OBJ_GROUP)
      fprintf(output, " \"%s\"", l->name);
  }
  if (!l->cpuset)
    return;
  if (loutput->show_cpuset == 1)
    fprintf(output, " cpuset=");
  if (loutput->show_cpuset) {
    char *cpusetstr;
    if (loutput->show_taskset)
      hwloc_bitmap_taskset_asprintf(&cpusetstr, l->cpuset);
    else
      hwloc_bitmap_asprintf(&cpusetstr, l->cpuset);
    fprintf(output, "%s", cpusetstr);
    free(cpusetstr);
  }

  /* annotate if the PU is forbidden/running */
  if (l->type == HWLOC_OBJ_PU && verbose_mode >= 2) {
    if (lstopo_pu_forbidden(l))
      fprintf(output, " (forbidden)");
    else if (lstopo_pu_running(loutput, l))
      fprintf(output, " (running)");
  }
}

/* Recursively output topology in a console fashion */
static void
output_topology (struct lstopo_output *loutput, hwloc_obj_t l, hwloc_obj_t parent, int i)
{
  FILE *output = loutput->file;
  int verbose_mode = loutput->verbose_mode;
  hwloc_obj_t child;
  int group_identical = (verbose_mode <= 1) && !loutput->show_cpuset;
  unsigned collapse = 1;

  if (l->type == HWLOC_OBJ_PCI_DEVICE) {
    const char *collapsestr = hwloc_obj_get_info_by_name(l, "lstopoCollapse");
    if (collapsestr)
      collapse = atoi(collapsestr);
    if (!collapse)
      return;
  }

  if (group_identical
      && parent && parent->arity == 1
      && l->cpuset && parent->cpuset && hwloc_bitmap_isequal(l->cpuset, parent->cpuset)) {
    /* in non-verbose mode, merge objects with their parent is they are exactly identical */
    fprintf(output, " + ");
  } else {
    if (parent)
      fprintf(output, "\n");
    indent (output, 2*i);
    i++;
  }

  if (collapse > 1)
    fprintf(output, "%u x { ", collapse);
  output_console_obj(loutput, l, collapse);
  if (collapse > 1)
    fprintf(output, " }");

  for(child = l->first_child; child; child = child->next_sibling)
    if (child->type != HWLOC_OBJ_PU || !loutput->ignore_pus)
      output_topology (loutput, child, l, i);
  for(child = l->io_first_child; child; child = child->next_sibling)
    output_topology (loutput, child, l, i);
  for(child = l->misc_first_child; child; child = child->next_sibling)
    output_topology (loutput, child, l, i);
}

/* Recursive so that multiple depth types are properly shown */
static void
output_only (struct lstopo_output *loutput, hwloc_obj_t l)
{
  FILE *output = loutput->file;
  hwloc_obj_t child;
  if (loutput->show_only == l->type) {
    output_console_obj (loutput, l, 0);
    fprintf (output, "\n");
  }
  for(child = l->first_child; child; child = child->next_sibling)
    output_only (loutput, child);
  if (loutput->show_only == HWLOC_OBJ_BRIDGE || loutput->show_only == HWLOC_OBJ_PCI_DEVICE
      || loutput->show_only == HWLOC_OBJ_OS_DEVICE || loutput->show_only == HWLOC_OBJ_MISC) {
    /* I/O can only contain other I/O or Misc, no need to recurse otherwise */
    for(child = l->io_first_child; child; child = child->next_sibling)
      output_only (loutput, child);
  }
  if (loutput->show_only == HWLOC_OBJ_MISC) {
    /* Misc can only contain other Misc, no need to recurse otherwise */
    for(child = l->misc_first_child; child; child = child->next_sibling)
      output_only (loutput, child);
  }
}

static void output_distances(struct lstopo_output *loutput)
{
  hwloc_topology_t topology = loutput->topology;
  int logical = loutput->logical;
  FILE *output = loutput->file;
  unsigned topodepth = hwloc_topology_get_depth(topology);
  unsigned depth;

  for (depth = 0; depth < topodepth; depth++) {
    unsigned nr, i;
    nr = hwloc_get_nbobjs_by_depth(topology, depth);
    for (i = 0; i < nr; i++) {
      hwloc_obj_t obj = hwloc_get_obj_by_depth(topology, depth, i);
      unsigned j;
      for (j = 0; j < obj->distances_count; j++) {
	const struct hwloc_distances_s * distances = obj->distances[j];
	char typestring[32];
	if (!distances || !distances->latency)
	  continue;
	hwloc_obj_type_snprintf (typestring, sizeof(typestring), obj, 1);
	fprintf(output, "Relative latency matrix between %u %ss (depth %u) by %s indexes (below %s%s%u):\n",
		distances->nbobjs,
		hwloc_type_name(hwloc_get_depth_type(topology, obj->depth + distances->relative_depth)),
		obj->depth + distances->relative_depth,
		logical ? "logical" : "physical",
		typestring,
		logical ? " L#" :  " P#",
		logical ? obj->logical_index : obj->os_index);
	hwloc_utils_print_distance_matrix(output, topology, obj, distances->nbobjs, distances->relative_depth, distances->latency, logical);
      }
    }
  }
}

void output_console(struct lstopo_output *loutput, const char *filename)
{
  hwloc_topology_t topology = loutput->topology;
  int verbose_mode = loutput->verbose_mode;
  FILE *output;

  output = open_output(filename, loutput->overwrite);
  if (!output) {
    fprintf(stderr, "Failed to open %s for writing (%s)\n", filename, strerror(errno));
    return;
  }
  loutput->file = output;

  if (loutput->show_distances_only) {
    output_distances(loutput);
    return;
  }

  /*
   * if verbose_mode == 0, only print the summary.
   * if verbose_mode == 1, only print the topology tree.
   * if verbose_mode > 1, print both.
   */

  if (loutput->show_only != HWLOC_OBJ_TYPE_NONE) {
    if (verbose_mode > 1)
      fprintf(output, "Only showing %s objects\n", hwloc_type_name(loutput->show_only));
    output_only (loutput, hwloc_get_root_obj(topology));
  } else if (verbose_mode >= 1) {
    output_topology (loutput, hwloc_get_root_obj(topology), NULL, 0);
    fprintf(output, "\n");
  }

  if ((verbose_mode > 1 || !verbose_mode) && loutput->show_only == HWLOC_OBJ_TYPE_NONE) {
    hwloc_lstopo_show_summary(output, topology);
  }

  if (verbose_mode > 1 && loutput->show_only == HWLOC_OBJ_TYPE_NONE) {
    output_distances(loutput);
  }

  if (verbose_mode > 1 && loutput->show_only == HWLOC_OBJ_TYPE_NONE) {
    hwloc_const_bitmap_t complete = hwloc_topology_get_complete_cpuset(topology);
    hwloc_const_bitmap_t topo = hwloc_topology_get_topology_cpuset(topology);
    hwloc_const_bitmap_t allowed = hwloc_topology_get_allowed_cpuset(topology);

    if (!hwloc_bitmap_isequal(topo, complete)) {
      hwloc_bitmap_t unknown = hwloc_bitmap_alloc();
      char *unknownstr;
      hwloc_bitmap_copy(unknown, complete);
      hwloc_bitmap_andnot(unknown, unknown, topo);
      hwloc_bitmap_asprintf(&unknownstr, unknown);
      fprintf (output, "%d processors not represented in topology: %s\n", hwloc_bitmap_weight(unknown), unknownstr);
      free(unknownstr);
      hwloc_bitmap_free(unknown);
    }
    if (!hwloc_bitmap_isequal(topo, allowed)) {
      hwloc_bitmap_t disallowed = hwloc_bitmap_alloc();
      char *disallowedstr;
      hwloc_bitmap_copy(disallowed, topo);
      hwloc_bitmap_andnot(disallowed, disallowed, allowed);
      hwloc_bitmap_asprintf(&disallowedstr, disallowed);
      fprintf(output, "%d processors represented but not allowed: %s\n", hwloc_bitmap_weight(disallowed), disallowedstr);
      free(disallowedstr);
      hwloc_bitmap_free(disallowed);
    }
    if (!hwloc_topology_is_thissystem(topology))
      fprintf (output, "Topology not from this system\n");
  }

  if (output != stdout)
    fclose(output);
}

void output_synthetic(struct lstopo_output *loutput, const char *filename)
{
  hwloc_topology_t topology = loutput->topology;
  FILE *output;
  int length;
  char sbuffer[1024];
  char * dbuffer = NULL;
  unsigned nb1, nb2, nb3;

  if (!hwloc_get_root_obj(topology)->symmetric_subtree) {
    fprintf(stderr, "Cannot output assymetric topology in synthetic format.\n");
    return;
  }

  nb1 = hwloc_get_nbobjs_by_type(topology, HWLOC_OBJ_MISC);
  if (nb1) {
    fprintf(stderr, "Ignoring %u Misc objects.\n", nb1);
    fprintf(stderr, "Passing --ignore Misc may remove them.\n");
  }
  nb1 = hwloc_get_nbobjs_by_type(topology, HWLOC_OBJ_BRIDGE);
  nb2 = hwloc_get_nbobjs_by_type(topology, HWLOC_OBJ_PCI_DEVICE);
  nb3 = hwloc_get_nbobjs_by_type(topology, HWLOC_OBJ_OS_DEVICE);
  if (nb1 || nb2 || nb3) {
    fprintf(stderr, "Ignoring %u Bridge, %u PCI device and %u OS device objects\n", nb1, nb2, nb3);
    fprintf(stderr, "Passing --no-io may remove them.\n");
  }

  length = hwloc_topology_export_synthetic(topology, sbuffer, sizeof(sbuffer), loutput->export_synthetic_flags);
  if (length < 0) {
    fprintf(stderr, "Failed to export a synthetic description (%s)\n", strerror(errno));
    return;
  }

  if (length >= (int) sizeof(sbuffer)) {
    dbuffer = malloc(length+1 /* \0 */);
    if (!dbuffer)
      return;

    length = hwloc_topology_export_synthetic(topology, dbuffer, length+1, loutput->export_synthetic_flags);
    if (length < 0)
      goto out;
  }

  output = open_output(filename, loutput->overwrite);
  if (!output) {
    fprintf(stderr, "Failed to open %s for writing (%s)\n", filename, strerror(errno));
    goto out;
  }

  fprintf(output, "%s\n", dbuffer ? dbuffer : sbuffer);

  if (output != stdout)
    fclose(output);

 out:
  free(dbuffer);
}
