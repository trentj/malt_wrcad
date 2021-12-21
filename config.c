// vi: ts=2 sts=2 sw=2 et tw=100
/* parse configuraton files */

// TODO
// - Keep track of initialized status of fields in Builder, don't print the uninitialized ones
// (print examples instead)
// - implement units field for dt and dx

#include "config.h"
#include "malt.h"
#include "toml.h"
#include <assert.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* for strcasecmp */
#include <unistd.h>

static const Param PARAM_DEFAULT = {
    .name = NULL,
    .nominal = 1,
    .sigma = 0.0,
    .sigabs = 0.0,
    .min = 0.5,
    .max = 2.0,
    .top_min = 0,
    .top_max = 0,
    .staticc = 0,
    .isnommin = 0,
    .isnommax = 0,
    .include = 1,
    .logs = 1,
    .corners = 0,
};

typedef struct builder {
  int function;
  char *command;
  FILE *log;

  struct file_names file_names;

  struct extensions extensions;

  struct options options;

  Node node_defaults;
  Param param_defaults;

  int num_nodes;
  Node *nodes;

  int num_params_all;
  Param *params;

  int num_2D;
  _2D *_2D;
} Builder;

static void builder_debug(const Builder *B, FILE *fp)
{
  int i;
  /* organize according to function */
  fprintf(fp, "***  Configuration File  ***\n");
  if (B->file_names.circuit)
    fprintf(fp, "*circuit file name: %s\n", B->file_names.circuit);
  if (B->file_names.param)
    fprintf(fp, "*parameter file name: %s\n", B->file_names.param);
  if (B->file_names.passf)
    fprintf(fp, "*passfail file name: %s\n", B->file_names.passf);
  if (B->file_names.envelope)
    fprintf(fp, "*envelope file name: %s\n", B->file_names.envelope);
  fprintf(fp, "\n***  Circuit Node Defaults  ***\n");
  fprintf(fp, "dt = %g\n", B->node_defaults.dt);
  fprintf(fp, "dx = %g\n", B->node_defaults.dx);
  fprintf(fp, "\n***  Circuit Parameter Defaults  ***\n");
  fprintf(fp, "nominal = %g\n", B->param_defaults.nominal);
  fprintf(fp, "min = %g\n", B->param_defaults.min);
  fprintf(fp, "max = %g\n", B->param_defaults.max);
  /* by default, both sigma and sig_abs are zero. but one-and-only-one must be non-zero in the end
   */
  fprintf(fp, "sigma = %g\n", B->param_defaults.sigma);
  fprintf(fp, "sig_abs = %g\n", B->param_defaults.sigabs);
  fprintf(fp, "static = %d\n", B->param_defaults.staticc);
  fprintf(fp, "logs = %d\n", B->param_defaults.logs);
  fprintf(fp, "corners = %d\n", B->param_defaults.corners);
  fprintf(fp, "include = %d\n", B->param_defaults.include);
  fprintf(fp, "\n***  Circuit Nodes  ***\n");
  for (i = 0; i < B->num_nodes; ++i)
    fprintf(fp, "node = %s, dt = %g, dx = %g\n", B->nodes[i].name, B->nodes[i].dt, B->nodes[i].dx);
  fprintf(fp, "\n***  Circuit Parameters  ***\n");
  for (i = 0; i < B->num_params_all; ++i) {
    fprintf(fp,
            "param = %s, nominal = %g, min = %g, max = %g, sigma = %g, "
            "sig_abs = %g, static = %d, logs = %d, corners = %d, include = %d",
            B->params[i].name, B->params[i].nominal, B->params[i].min, B->params[i].max,
            B->params[i].sigma, B->params[i].sigabs, B->params[i].staticc, B->params[i].logs,
            B->params[i].corners, B->params[i].include);
    if (B->params[i].isnommin)
      fprintf(fp, ", nom_min = %g", B->params[i].nom_min);
    if (B->params[i].isnommax)
      fprintf(fp, ", nom_max = %g", B->params[i].nom_max);
    fprintf(fp, "\n");
  }
  fprintf(fp, "\n***  2D Margins  ***\n");
  for (i = 0; i < B->num_2D; ++i)
    fprintf(fp, "param_x = %s, param_y = %s\n", B->_2D[i].name_x, B->_2D[i].name_y);
  /* fake line as an example */
  if (B->num_2D == 0) {
    fprintf(fp, "***  Example. Replace arguments with actual param names  ***\n");
    fprintf(fp, "* param_x = %s, param_y = %s\n", "XJ", "XL");
  }
  fprintf(fp, "\n***  File Extensions  ***\n");
  if (B->extensions.circuit)
    fprintf(fp, "circuit_extension = %s\n", B->extensions.circuit);
  if (B->extensions.param)
    fprintf(fp, "parameters_extension = %s\n", B->extensions.param);
  if (B->extensions.passf)
    fprintf(fp, "passfail_extension = %s\n", B->extensions.passf);
  if (B->extensions.envelope)
    fprintf(fp, "envelope_extension = %s\n", B->extensions.envelope);
  if (B->extensions.plot)
    fprintf(fp, "plot_extension = %s\n", B->extensions.plot);
  fprintf(fp, "\n***  General Options  ***\n");
  fprintf(fp, "*fraction of sigma:\nbinsearch_accuracy = %g\n", B->options.binsearch_accuracy);
  if (B->options.spice_call_name)
    fprintf(fp, "spice_call_name = %s\n", B->options.spice_call_name);
  fprintf(fp, "max_subprocesses = %d\n", B->options.max_subprocesses);
  fprintf(fp, "threads = %d\n", B->options.threads);
  fprintf(fp, "verbose = %d\n", B->options.spice_verbose);
  fprintf(fp, "print_terminal = %d\n", B->options.print_terminal);
  fprintf(fp, "\n***  Options for Define  ***\n");
  fprintf(fp, "d_simulate = %d\n", B->options.d_simulate);
  fprintf(fp, "d_envelope = %d\n", B->options.d_envelope);
  fprintf(fp, "\n***  Options for Margins  ***\n");
  fprintf(fp, "\n***  Options for 2D Margins  ***\n");
  fprintf(fp, "2D_iter = %d\n", B->options._2D_iter);
  /* TODO not yet supported
  fprintf(fp,"\n***  Options for 2D Shmoo  ***\n");
  */
  fprintf(fp, "\n***  Options for Corners Yield  ***\n");
  fprintf(fp, "***  ranges: 0-10, 0-9, 1-40\n");
  fprintf(fp, "y_search_depth = %d\n", B->options.y_search_depth);
  fprintf(fp, "y_search_width  = %d\n", B->options.y_search_width);
  fprintf(fp, "y_search_steps  = %d\n", B->options.y_search_steps);
  fprintf(fp, "y_max_mem_k = %d\n", B->options.y_max_mem_k);
  fprintf(fp, "y_accuracy = %g\n", B->options.y_accuracy);
  fprintf(fp, "y_print_every = %d\n", B->options.y_print_every);
  fprintf(fp, "\n***  Options for Optimize  ***\n");
  fprintf(fp, "o_min_iter = %d\n", B->options.o_min_iter);
  fprintf(fp, "o_max_mem_k = %d\n", B->options.o_max_mem_k);
}

static void _2D_drop(_2D *ptr)
{
  free((void *)ptr->name_x);  // mem:schizzo
  ptr->name_x = NULL;
  free((void *)ptr->name_y);  // mem:foreconsent
  ptr->name_y = NULL;
}

static void param_drop(Param *ptr)
{
  free((void *)ptr->name);  // mem:reminiscer
  ptr->name = NULL;
}

static void node_drop(Node *nptr)
{
  free((void *)nptr->name);  // mem:sunglow
  nptr->name = NULL;
}

void freeConfiguration(Configuration *C)
{
  free(C->file_names.circuit);               // mem:tectospondylous
  free(C->file_names.param);                 // mem:stairwork
  free(C->file_names.passf);                 // mem:toners
  free(C->file_names.envelope);              // mem:cool
  free(C->file_names.config);                // mem:synurae
  free(C->file_names.env_call);              // mem:semishady
  free(C->file_names.plot);                  // mem:federalizes
  free(C->file_names.iter);                  // mem:myrcene
  free(C->file_names.pname);                 // mem:physnomy
  free(C->extensions.which_trace);           // mem:intratubal
  free((void *)C->options.spice_call_name);  // mem:descendentalism

  /* FIXME: add this part when we can guarantee log has been opened ~ntj
  fclose(C->log);
  */

  for (int n = 0; n < C->num_nodes; ++n) {
    node_drop((Node *)&C->nodes[n]);
  }
  free((void *)C->nodes);  // mem:anathematise

  for (int p = 0; p < C->num_params_all; ++p) {
    param_drop((Param *)&C->params[p]);
  }
  free((void *)C->params);  // mem:restringer

  for (int t = 0; t < C->num_2D; ++t) {
    _2D_drop(&C->_2D[t]);
  }
  free(C->_2D);  // mem:hypersexual

  free(C);  // mem:circumgyrate
}

static void builder_init(Builder *C, const Args *args, FILE *log)
{
  // the following fields are always initialized so that they are available for debugging:
  C->function = args->function;
  C->command = args->circuit_name;
  C->log = log;
  /* initialize everything to internal defaults */
  /* internal file names */
  C->file_names.circuit = NULL;
  C->file_names.param = NULL;
  C->file_names.passf = NULL;
  C->file_names.envelope = NULL;
  C->file_names.config = NULL;
  C->file_names.env_call = NULL;
  C->file_names.plot = NULL;
  C->file_names.iter = NULL;
  C->file_names.pname = NULL;
  /* node defaults */
  C->node_defaults.name = NULL;
  C->node_defaults.units = 'V';
  C->node_defaults.dt = 100e-12;
  C->node_defaults.dx = 1;
  /* parameter defaults */
  C->param_defaults = PARAM_DEFAULT;
  /* nodes, params, and 2D */
  C->nodes = NULL;
  C->params = NULL;
  C->_2D = NULL;
  C->num_nodes = 0;
  C->num_params_all = 0;
  C->num_2D = 0;
  /* extensions */
  C->extensions.circuit = ".cir";
  C->extensions.param = ".param";
  C->extensions.passf = ".passf";
  C->extensions.envelope = ".envelope";
  C->extensions.config = ".toml";
  C->extensions.plot = ".plot";
  /* pretty weak */
  C->extensions.which_trace = malloc(LINE_LENGTH);  // mem:intratubal
  /* options */
  C->options.binsearch_accuracy = 0.1;
  C->options.spice_call_name = strdup("wrspice");  // mem:descendentalism
  C->options.spice_verbose = 0;
  C->options.threads = 16;          // default # threads: 16
  C->options.max_subprocesses = 0;  // default # jobs: unlimited
  C->options.print_terminal = 1;
  /* options for define */
  C->options.d_simulate = 1;
  C->options.d_envelope = 1;
  /* options for margins */
  /* options for 2D margins */
  C->options._2D_iter = 16;
  /* TODO: options for 2D shmoo */
  /* options for corners yield */
  C->options.y_search_depth = 5;
  C->options.y_search_width = 5;
  C->options.y_search_steps = 12;
  C->options.y_max_mem_k = 4194304;
  C->options.y_accuracy = 10;
  C->options.y_print_every = 0;
  /* options for optimize */
  C->options.o_min_iter = 100;
  C->options.o_max_mem_k = 4194304;
}

/* Reads a boolean from the table `values`, allowing either a TOML boolean or
 * integer value.
 * Returns 1 and stores the result in *dest if present; returns 0 otherwise. */
// TODO: replace int -> bool
__attribute__((nonnull)) static int read_a_bool(int *dest, toml_table_t *values, const char *key)
{
  toml_datum_t value = toml_bool_in(values, key);
  if (value.ok) {
    *dest = value.u.b;
  } else if ((value = toml_int_in(values, key)).ok) {
    *dest = value.u.i;
  }
  return *dest;
}

/* Reads an integer from the table `values`.
 * Returns 1 and stores the result in *dest if present; returns 0 otherwise. */
__attribute__((nonnull)) static int read_an_int(int *dest, toml_table_t *values, const char *key)
{
  toml_datum_t value = toml_int_in(values, key);
  if (value.ok) {
    *dest = value.u.i;
    return 1;
  } else {
    return 0;
  }
}

/* Read a `double` from the `values` table.
 * Returns 1 and stores the result in *dest when successful, 0 otherwise. */
__attribute__((nonnull)) static int read_a_double(double *dest, toml_table_t *values,
                                                  const char *key)
{
  toml_datum_t value = toml_double_in(values, key);
  if (value.ok) {
    *dest = value.u.d;
    return 1;
  } else {
    return 0;
  }
}

/* Reads a string (char *) from the table `values`.
 * Returns 1 and stores the result in *dest if successful, 0 otherwise.
 * `*dest` needs to be freed. */
__attribute__((nonnull)) static int read_a_string(const char **dest, toml_table_t *values,
                                                  const char *key)
{
  toml_datum_t ext = toml_string_in(values, key);  // mem:timetaker
  if (ext.ok) {
    *dest = ext.u.s;
    return 1;
  } else {
    return 0;
  }
}

/* Parse the [envelope] section of this file.
 * Returns 0 if the section is not present or incomplete and 1 otherwise. */
static int read_envelope(Builder *C, toml_table_t *t)
{
  // read [envelope] table (NOTE: must be done before nodes)
  toml_table_t *envelope = toml_table_in(t, "envelope");
  if (envelope) {
    // TODO: parse units?
    return read_a_double(&C->node_defaults.dx, envelope, "dx") &&
           read_a_double(&C->node_defaults.dt, envelope, "dt");
  } else {
    return 0;
  }
}

/* Read the `nodes` list from the top level of the toml file. Also reads [envelope] if nodes are
 * present.
 * Returns the number of nodes successfully read. */
static int read_nodes(Builder *C, toml_table_t *t)
{
  int len_nodes = 0;
  int has_envelope = read_envelope(C, t);
  toml_array_t *nodes = toml_array_in(t, "nodes");
  if (nodes) {
    // nodes are present: [envelope] must be too, for defaults
    if (!has_envelope) {
      error("Missing [envelope] in TOML file containing nodes list\n");
    }
    len_nodes = toml_array_nelem(nodes);
    // overwrite nodes if already specified by earlier configurations
    // FIXME: clobbering leaks memory for .name
    C->nodes = realloc(C->nodes, len_nodes * sizeof *C->nodes);
    if (C->num_nodes != 0) {
      warn("Overwriting previously configured node list\n");
    }
    C->num_nodes = len_nodes;
    for (int n = 0; n < len_nodes; ++n) {
      toml_datum_t node = toml_string_at(nodes, n);  // mem:sunglow
      if (node.ok) {
        // node is a string; we will use the provided envelope settings
        // add this node to the configuration
        C->nodes[n].name = node.u.s;
        C->nodes[n].units = C->node_defaults.units;
        C->nodes[n].dt = C->node_defaults.dt;
        C->nodes[n].dx = C->node_defaults.dx;
      } else {
        toml_table_t *node = toml_table_at(nodes, n);
        if (node) {
          // TODO: permit nodes to have metadata, and check that dx, dt are nonzero
          error("Table not supported for node %zu (this is a bug in Malt)\n", n);
        } else {
          error("Node %zu is neither a string nor a table\n", n);
        }
      }
    }
  }
  return len_nodes;
}

/* Read the [parameters] table from the TOML file, if present.
 * Returns the number of parameters successfully read.  */
static int read_parameters(Builder *C, toml_table_t *t)
{
  toml_table_t *parameters = toml_table_in(t, "parameters");
  if (parameters) {
    // loop over the table entries
    int i;
    for (i = 0;; ++i) {
      const char *parameter = toml_key_in(parameters, i);
      if (!parameter)
        break;
      toml_table_t *values = toml_table_in(parameters, parameter);
      if (!values) {
        error("[parameters.%s] is not a table\n", parameter);
      }

      // does this parameter already exist?
      bool match = false;
      int n;
      for (n = 0; n < C->num_params_all; ++n) {
        if (!strcmp(C->params[n].name, parameter)) {
          match = true;
          break;
        }
      }
      if (!match) {
        // grow C->params by 1 (slow but whatever)
        C->params =
            realloc(C->params, (++C->num_params_all) * sizeof *C->params);  // mem:restringer
        C->params[n] = PARAM_DEFAULT;
      }

      // populate/overwrite C->params[n] with the contents of `values`
      // parameter name:
      C->params[n].name = strdup(parameter);  // mem:reminiscer

      // nominal (required field):
      if (!read_a_double(&C->params[n].nominal, values, "nominal")) {
        error("Parameter '%s' has no nominal value\n", parameter);
      }

      // sigma, sigabs:
      // TODO: parse a subtable {percent = 5} or whatever
      toml_datum_t sigma = toml_double_in(values, "sigma");
      toml_datum_t sigabs = toml_double_in(values, "sig_abs");
      if (sigma.ok && !sigabs.ok) {
        C->params[n].sigma = sigma.u.d;
      } else if (sigabs.ok && !sigma.ok) {
        C->params[n].sigabs = sigabs.u.d;
      } else {
        error("Parameter '%s' must have exactly one of sigma or sig_abs\n", parameter);
        return 0;
      }

      // min, max:
      C->params[n].top_min |= read_a_double(&C->params[n].min, values, "min");
      C->params[n].top_max |= read_a_double(&C->params[n].max, values, "max");

      // nom_min, nom_max:
      C->params[n].isnommin |= read_a_double(&C->params[n].nom_min, values, "nom_min");
      C->params[n].isnommax |= read_a_double(&C->params[n].nom_max, values, "nom_max");

      // various flags:
      read_a_bool(&C->params[n].staticc, values, "static");
      read_a_bool(&C->params[n].include, values, "include");
      read_a_bool(&C->params[n].logs, values, "logs");
      read_a_bool(&C->params[n].corners, values, "corners");
    }
    return i;
  }
  info("No [parameters] in config file\n");
  return 0;
}

/* Reads the [extensions] table containing file extensions from the TOML file, if present.
 * Returns the number of extensions successfully read. */
static int read_extensions(Builder *C, toml_table_t *t)
{
  toml_table_t *extensions = toml_table_in(t, "extensions");
  int n = 0;
  if (extensions) {
    n += read_a_string(&C->extensions.circuit, extensions, "circuit");    // mem:timetaker
    n += read_a_string(&C->extensions.param, extensions, "parameters");   // mem:rupa
    n += read_a_string(&C->extensions.passf, extensions, "passfail");     // mem:essentia
    n += read_a_string(&C->extensions.envelope, extensions, "envelope");  // mem:trinkle
    n += read_a_string(&C->extensions.plot, extensions, "plot");          // mem:irrepairable
  }
  return n;
}

/* Reads the [define] table (define options) from the TOML file, if present.
 * Returns the number of key-value pairs successfully converted. */
static int read_define_opts(Builder *C, toml_table_t *t)
{
  toml_table_t *define = toml_table_in(t, "define");
  int n = 0;
  n += read_a_bool(&C->options.d_simulate, define, "simulate");
  n += read_a_bool(&C->options.d_envelope, define, "envelope");
  return n;
}

/* Reads the [yield] table (corners-yield options) from the TOML file, if present.
 * Returns the number of key-value pairs successfully converted. */
static int read_yield_opts(Builder *C, toml_table_t *t)
{
  toml_table_t *yield = toml_table_in(t, "yield");
  int n = 0;
  n += read_an_int(&C->options.y_search_depth, yield, "search_depth");
  n += read_an_int(&C->options.y_search_width, yield, "search_width");
  n += read_an_int(&C->options.y_search_steps, yield, "search_steps");
  // TODO: parse a subtable {k: 4194304} or something instead of units in the name (also in
  // optimize)
  n += read_an_int(&C->options.y_max_mem_k, yield, "max_mem_k");
  n += read_a_double(&C->options.y_accuracy, yield, "accuracy");
  // TODO: check that print_every is working optimally
  n += read_an_int(&C->options.y_print_every, yield, "print_every");
  return n;
}

/* Reads the [optimize] table (optimization options) from the TOML file, if present.
 * Returns the number of key-value pairs successfully converted. */
static int read_optimize_opts(Builder *C, toml_table_t *t)
{
  toml_table_t *optimize = toml_table_in(t, "optimize");
  int n = 0;
  n += read_an_int(&C->options.o_min_iter, optimize, "min_iter");
  n += read_an_int(&C->options.o_min_iter, optimize, "max_mem_k");
  return n;
}

/* Reads the [xy] table (2D sweep settings) from the TOML file, if present.
 * Returns the number of sweeps successfully converted. */
static int read_xy_sweeps(Builder *C, toml_table_t *t)
{
  toml_table_t *xy = toml_table_in(t, "xy");
  int len_sweeps = 0;
  if (xy) {
    toml_array_t *sweeps = toml_array_in(xy, "sweeps");
    if (sweeps) {
      len_sweeps = toml_array_nelem(sweeps);
      // overwrite old sweeps
      // FIXME: clobbering leaks memory for .name_x, .name_y
      C->_2D = realloc(C->_2D, len_sweeps * sizeof *C->_2D);  // mem:hypersexual
      if (C->num_2D != 0) {
        warn("Overwriting previously configured 2D sweeps\n");
      }
      C->num_2D = len_sweeps;
      for (int n = 0; n < len_sweeps; ++n) {
        toml_table_t *sweep = toml_table_at(sweeps, n);
        if (!sweep) {
          error("2D sweep #%d is not a table\n", n);
        }
        toml_datum_t x = toml_string_in(sweep, "x");  // mem:schizzo
        toml_datum_t y = toml_string_in(sweep, "y");  // mem:foreconsent
        if (x.ok && y.ok) {
          C->_2D[n].name_x = x.u.s;
          C->_2D[n].name_y = y.u.s;
        } else {
          error("2D sweep #%d must define both x and y parameters\n", n);
        }
      }
    }
    toml_datum_t iterations = toml_int_in(xy, "iterations");
    if (iterations.ok) {
      C->options._2D_iter = iterations.u.i;
    }
  }
  return len_sweeps;
}

/* If there is a file at *filename, open and parse it.
 * Returns 1 if the file was successfully parsed, 0 if the file does not exist; aborts otherwise. */
static int try_parse_file(Builder *C, char *filename)
{
  FILE *fp = fopen(filename, "r");

  if (!fp) {
    return 0;
  }

  char errbuf[200];
  toml_table_t *t = toml_parse_file(fp, errbuf, sizeof errbuf);
  fclose(fp);
  if (!t) {
    error("Cannot parse TOML: %s\n", errbuf);
  }

  // options:
  // TODO: reconcile verbosity from command line and configs
  read_a_bool(&C->options.spice_verbose, t, "verbose");
  read_an_int(&C->options.max_subprocesses, t, "max_subprocesses");
  assert(C->options.max_subprocesses >= 0);  // 0 means unlimited, negative is illegal
  read_an_int(&C->options.threads, t, "threads");
  assert(C->options.threads > 0);  // number of threads must be at least 1
  // TODO: check that print_terminal is working as intended
  read_a_bool(&C->options.print_terminal, t, "print_terminal");
  read_a_double(&C->options.binsearch_accuracy, t, "binsearch_accuracy");
  // FIXME: I think this leaks memory when successful
  read_a_string(&C->options.spice_call_name, t, "spice_call_name");

  read_nodes(C, t);
  read_parameters(C, t);
  read_extensions(C, t);
  read_define_opts(C, t);
  // read_margins_opts(C, t);
  // read_trace_opts(C, t);
  read_yield_opts(C, t);
  read_optimize_opts(C, t);
  read_xy_sweeps(C, t);

  toml_free(t);

  return 1;
}

/* List paths that are candidates for having a Malt.toml in the current directory (`path`) and all
 * parent directories up to either / or the user's $HOME.
 *
 * Sets `*listp` to an array of pointers to paths. Returns the number of paths. Both the array and
 * the individual paths need to be freed.  */
static int config_file_candidates(char ***listp, char *path)
{
  const char *home = getenv("HOME");
  int len = 0, cap = 10;
  if (listp != NULL) {
    *listp = malloc(cap * sizeof **listp);  // mem:nonaddicted
  }

  char *sptr = &path[strlen(path)];

  while (sptr) {
    // walk sptr backwards to the first in any run of separators (e.g. "/////")
    while (sptr > path && sptr[-1] == '/') {
      --sptr;
    }
    // truncate path to the last '/'
    *sptr = '\0';

    if (listp != NULL) {
      // reallocate if necessary
      if (len == cap) {
        cap += cap / 2;
        *listp = realloc(*listp, cap * sizeof **listp);  // mem:nonaddicted
        assert(*listp);
      }

      // add path/Malt.toml to the candidate list
      (*listp)[len] = resprintf(NULL, "%s/Malt.toml", path);  // mem:hypophloeodal
      ++len;
    }

    // if at $HOME, stop
    if ((home != NULL) && (strcmp(home, path) == 0)) {
      break;
    }

    // move sptr back to the last occurrence of '/'
    sptr = strrchr(path, '/');
  }
  return len;
}

/* Reorders `C->params` so that its elements appear in this order:
 * 1. all the included, corners=0 parameters
 * 2. all the included, corners=1 parameters
 * 3. all non-included parameters. */
static void exclude_params_corn(Configuration *C)
{
#define include_now(i) (C->params[i].include && !C->params[i].corners)
  /* bubble sort */
  for (int i = C->num_params_all; 0 < i; --i) {
    for (int j = 1; i > j; ++j) {
      if (C->params[j - 1].include < C->params[j].include) {
        // swap:
        Param bubba = C->params[j];
        C->params[j] = C->params[j - 1];
        C->params[j - 1] = bubba;
      }
    }
  }
  /* bubble sort again */
  for (int i = C->num_params_all; 0 < i; --i) {
    for (int j = 1; i > j; ++j) {
      if (include_now(j - 1) < include_now(j)) {
        // swap:
        Param bubba = C->params[j];
        C->params[j] = C->params[j - 1];
        C->params[j - 1] = bubba;
      }
    }
  }
  C->num_params = C->num_params_corn = 0;
  for (int i = 0; C->num_params_all > i; ++i) {
    if (C->params[i].include) {
      if (C->params[i].corners) {
        // count included params that are corners
        ++C->num_params_corn;
      } else {
        // count included normal parameters
        ++C->num_params;
      }
    }
  }
#undef include_now
}

/* Finds the longest filename that starts with `prefix`, ends with `extension`, and refers to a
 * file that exists. Stores the result in `*filename`.  */
static void most_specific_filename(char *filename, const char *extension, const char *prefix)
{
  char *file_prefix = strdup(prefix);  // mem:watchless
  char *temp = NULL;
  for (;;) {
    resprintf(&temp, "%s%s", file_prefix, extension);  // mem:tautonymic
    FILE *fd;
    if ((fd = fopen(temp, "r"))) {
      strcpy(filename, temp);
      fclose(fd);
      break;
    } else {
      char *last_dot = strrchr(file_prefix, '.');
      if (last_dot == NULL) {
        /* TODO: figure out what happens if this is never fixed */
        filename[0] = '\0';
        break;
      } else {
        *last_dot = '\0';
      }
    }
  }
  free(file_prefix);  // mem:watchless
  free(temp);         // mem:tautonymic
}

/* Move data from the builder into the configuration. */
void build_configuration(Configuration *C, Builder *B)
{
  C->function = B->function;
  C->command = B->command;
  C->log = B->log;
  C->file_names = B->file_names;
  C->options = B->options;
  C->extensions = B->extensions;
  // drop default node & parameter data
  node_drop(&B->node_defaults);
  param_drop(&B->param_defaults);
  C->num_nodes = B->num_nodes;
  C->nodes = B->nodes;
  C->num_params_all = B->num_params_all;
  C->params = B->params;
  C->num_2D = B->num_2D;
  C->_2D = B->_2D;
  // remove excluded params from the running (initializing num_params_corn and num_params)
  exclude_params_corn(C);
}

/* Finds and parses all applicable configuration files.
 *
 * There are two kinds of configuration files that may apply:
 * - Malt.toml files, which may be found in the current directory or any parent up to the user's
 *   home directory or /. All of these files are parsed.
 * - Run-specific <circuit>.toml, where <circuit> is a dotted list of identifiers, which are found
 *   only in the current directory. Only the most specific file (longest prefix of
 *   args.circuit_name) is parsed.  (This is a bug.) */
Configuration *Configure(const Args *args, FILE *log)
{
  FILE *fd, *fp;
  int levels, files;
  Builder B;
  Builder *C = &B;  // so we can use info, warn, error macros which assume a pointer

  builder_init(&B, args, log);

  if (args->verbosity > 0) {
    info("Parsing the Malt.toml file "
         "and the run-specific .toml file\n");
  }

  char *cwd = getcwd(NULL, 0);
  char **filelist = NULL;
  files = config_file_candidates(&filelist, cwd);
  free(cwd);

  bool some = false;
  for (levels = files - 1; levels >= 0; --levels) {
    if (try_parse_file(&B, filelist[levels])) {
      info("Parsed: '%s'\n", filelist[levels]);
      some = true;
    }
    free(filelist[levels]);  // mem:hypophloeodal
  }
  free(filelist);  // mem:nonaddicted
  if (!some) {
    /* Trigger Configuration Setup */
    warn("No Malt.toml configuration files found\n");
    if ((fp = fopen("Malt.toml", "w")) != NULL) {
      info("Generating a default configuration file './Malt.toml'\n");
    } else {
      error("Cannot open './Malt.toml' for writing\n");
    }
    builder_debug(&B, fp);
    fclose(fp);
  }
  /* file_name memory allocation */
  /* play it safe and simple */
  size_t stringy = strlen(B.options.spice_call_name) + strlen(B.command) +
                   strlen(B.extensions.circuit) + strlen(B.extensions.param) +
                   strlen(B.extensions.passf) + strlen(B.extensions.envelope) +
                   strlen(B.extensions.config) + strlen(B.extensions.plot) + 32;
  B.file_names.circuit = malloc(stringy);   // mem:tectospondylous
  B.file_names.param = malloc(stringy);     // mem:stairwork
  B.file_names.passf = malloc(stringy);     // mem:toners
  B.file_names.envelope = malloc(stringy);  // mem:cool
  B.file_names.config = malloc(stringy);    // mem:synurae
  B.file_names.env_call = malloc(stringy);  // mem:semishady
  B.file_names.plot = malloc(stringy);      // mem:federalizes
  B.file_names.iter = malloc(stringy);      // mem:myrcene
  B.file_names.pname = malloc(stringy);     // mem:physnomy
  /* the circuit, param, and envelope files we will be using */
  /* determined from the command line */

#define find_most_specific_filename(purpose) \
  most_specific_filename(B.file_names.purpose, B.extensions.purpose, B.command)
  find_most_specific_filename(circuit);
  find_most_specific_filename(param);
  find_most_specific_filename(envelope);
  find_most_specific_filename(config);
  find_most_specific_filename(passf);
#undef find_most_specific_filename

  /* parse the config file */
  if (try_parse_file(&B, B.file_names.config)) {
    info("Parsed '%s'\n", B.file_names.config);
  }

  /* write the final configuration to the final configuration file */
  /* overwrite the file if it already exists */
  char *filename_temp = NULL;
  resprintf(&filename_temp, "%s%s.%c", B.command, B.extensions.config, B.function);
  if ((fd = fopen(filename_temp, "w"))) {
    builder_debug(&B, fd);
    fclose(fd);
    info("Configuration written to '%s'\n", filename_temp);
  } else {
    warn("Cannot open '%s' for writing\n", filename_temp);
  }
  free(filename_temp);

  Configuration *cfg = malloc(sizeof *cfg);  // mem:circumgyrate
  build_configuration(cfg, &B);

  return cfg;
}
