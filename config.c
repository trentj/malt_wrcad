// vi: ts=2 sts=2 sw=2 et tw=100
/* parse configuraton files */

// TODO:
// - Keep track of initialized status of fields in Builder, don't print the uninitialized ones by
// value (print examples instead)
// - implement units field for dt and dx
// - separate [corners]

#include "config.h"
#include "list.h"
#include "malt.h"
#include "toml.h"
#include <assert.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* for strcasecmp */
#include <sys/stat.h>
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
  FILE *log;

  list_t project_tree;
  list_t working_tree;

  struct file_names file_names;

  bool keep_files;

  struct extensions extensions;

  struct options options;

  bool has_envelope;
  Node node_defaults;

  int num_nodes;
  Node *nodes;

  int num_params_all;
  Param *params;

  int num_2D;
  _2D *_2D;
} Builder;

static void builder_debug(const Builder *B, FILE *fp)
{
#define brk() fputc('\n', fp);
#define comment(format, ...) fprintf(fp, "# " format "\n", ##__VA_ARGS__)
#define section(section) fprintf(fp, "[" section "]\n")
#define key_val(key, value, ...) fprintf(fp, key " = " value "\n", ##__VA_ARGS__)
  /* organize according to function */
  fprintf(fp, "### Malt Configuration File ###\n");
  comment("Generated by Malt " MALTVERSION);
  if (B->file_names.circuit)
    comment("circuit file name: %s", B->file_names.circuit);
  if (B->file_names.param)
    comment("parameter file name: %s", B->file_names.param);
  if (B->file_names.passf)
    comment("passfail file name: %s", B->file_names.passf);
  if (B->file_names.envelope)
    comment("envelope file name: %s", B->file_names.envelope);

  brk();
  comment("General Options");
  comment("(These options must precede any [section] in this file)");
  key_val("binsearch_accuracy", "%g  # fraction of sigma", B->options.binsearch_accuracy);
  key_val("print_terminal", "%s", B->options.print_terminal ? "true" : "false");
  comment("Nodes may be specified as strings or tables.");
  comment("If dx or dt is not provided, [envelope] will be used instead.");
  comment("Example:");
  comment("nodes = [\"v(phi.node0)\", { node = \"v(phi.node1)\", dt = 1e-10, dx = 1.0 }]");
  fprintf(fp, "nodes = [");
  for (int i = 0; i < B->num_nodes; ++i) {
    if (B->nodes[i].dt == B->node_defaults.dt && B->nodes[i].dx == B->node_defaults.dx) {
      // string
      fprintf(fp, "\n  '%s',", B->nodes[i].name);
    } else {
      // inline table
      fprintf(fp, "\n  {node = '%s', dt = %g, dx = %g},", B->nodes[i].name, B->nodes[i].dt,
              B->nodes[i].dx);
    }
  }
  fprintf(fp, "%s]\n", B->num_nodes == 0 ? "" : "\n");

  brk();
  comment("Simulator options");
  section("simulator");
  key_val("max_subprocesses", "%d", B->options.max_subprocesses);
  key_val("command", "'%s'", B->options.spice_call_name);
  key_val("verbose", "%s", B->options.spice_verbose ? "true" : "false");

  brk();
  comment("Default envelope settings for all nodes");
  section("envelope");
  key_val("dt", "%g", B->node_defaults.dt);
  key_val("dx", "%g", B->node_defaults.dx);

  brk();
  comment("Circuit parameters");
  comment("Defaults: nominal = %g, min = %g, max = %g, sigma = %g, sig_abs = %g, static = %s, logs "
          "= %s, corners = %s, include = %s",
          PARAM_DEFAULT.nominal, PARAM_DEFAULT.min, PARAM_DEFAULT.max, PARAM_DEFAULT.sigma,
          PARAM_DEFAULT.sigabs, PARAM_DEFAULT.staticc ? "true" : "false",
          PARAM_DEFAULT.logs ? "true" : "false", PARAM_DEFAULT.corners ? "true" : "false",
          PARAM_DEFAULT.include ? "true" : "false");
  section("parameters");
  for (int i = 0; i < B->num_params_all; ++i) {
    fprintf(fp, "'%s' = { nominal = %g", B->params[i].name, B->params[i].nominal);
    if (B->params[i].top_min)
      fprintf(fp, ", min = %g", B->params[i].min);
    if (B->params[i].top_max)
      fprintf(fp, ", max = %g", B->params[i].max);
    if (B->params[i].sigma != 0.0)
      fprintf(fp, ", sigma = %g", B->params[i].sigma);
    if (B->params[i].sigabs != 0.0)
      fprintf(fp, ", sig_abs = %g", B->params[i].sigabs);
    if (B->params[i].staticc != PARAM_DEFAULT.staticc)
      fprintf(fp, ", static = %s", B->params[i].staticc ? "true" : "false");
    if (B->params[i].isnommin)
      fprintf(fp, ", nom_min = %g", B->params[i].nom_min);
    if (B->params[i].isnommax)
      fprintf(fp, ", nom_max = %g", B->params[i].nom_max);
    if (B->params[i].logs != PARAM_DEFAULT.logs)
      fprintf(fp, ", logs = %s", B->params[i].logs ? "true" : "false");
    if (B->params[i].corners != PARAM_DEFAULT.corners)
      fprintf(fp, ", corners = %s", B->params[i].corners ? "true" : "false");
    if (B->params[i].include != PARAM_DEFAULT.include)
      fprintf(fp, ", include = %s", B->params[i].include ? "true" : "false");
    fprintf(fp, " }\n");
  }

  brk();
  comment("Options for 2D margin analysis");
  section("xy");
  key_val("iterations", "%d", B->options._2D_iter);
  fprintf(fp, "sweeps = [\n");
  fprintf(fp, "  # Example:\n");
  fprintf(fp, "  # {x = \"Xj\", y = \"Xl\"},\n");
  for (int i = 0; i < B->num_2D; ++i) {
    fprintf(fp, "  {x = '%s', y = '%s'},\n", B->_2D[i].name_x, B->_2D[i].name_y);
  }
  fprintf(fp, "]\n");

  brk();
  comment("File extensions used by Malt");
  section("extensions");
  if (B->extensions.circuit)
    key_val("circuit", "'%s'", B->extensions.circuit);
  if (B->extensions.param)
    key_val("parameters", "'%s'", B->extensions.param);
  if (B->extensions.passf)
    key_val("passfail", "'%s'", B->extensions.passf);
  if (B->extensions.envelope)
    key_val("envelope", "'%s'", B->extensions.envelope);
  if (B->extensions.env_call)
    key_val("env_call", "'%s'", B->extensions.env_call);
  if (B->extensions.plot)
    key_val("plot", "'%s'", B->extensions.plot);

  brk();
  comment("Options for defining correct circuit operation");
  section("define");
  key_val("simulate", "%s", B->options.d_simulate ? "true" : "false");
  key_val("envelope", "%s", B->options.d_envelope ? "true" : "false");

  brk();
  comment("No options are currently supported for 1D margin analysis");
  comment("[margins]");

  brk();
  comment("Options for yield analysis");
  section("yield");
  comment("range: 0-10");
  key_val("search_depth", "%d", B->options.y_search_depth);
  comment("range: 0-9");
  key_val("search_width", "%d", B->options.y_search_width);
  comment("range: 1-40");
  key_val("search_steps", "%d", B->options.y_search_steps);
  key_val("max_mem_k", "%d", B->options.y_max_mem_k);
  key_val("accuracy", "%g", B->options.y_accuracy);
  key_val("print_every", "%d", B->options.y_print_every);

  brk();
  comment("Options for parameter optimization");
  section("optimize");
  key_val("min_iter", "%d", B->options.o_min_iter);
  key_val("max_mem_k", "%d", B->options.o_max_mem_k);
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
  free(C->file_names.env_call);              // mem:semishady
  free(C->file_names.plot);                  // mem:federalizes
  free(C->file_names.iter);                  // mem:myrcene
  free(C->file_names.pname);                 // mem:physnomy
  free(C->extensions.which_trace);           // mem:intratubal
  free((void *)C->options.spice_call_name);  // mem:descendentalism

  fclose(C->log);

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
  C->log = log;
  C->keep_files = args->keep_files;
  /* initialize everything to internal defaults */
  C->has_envelope = false;
  C->project_tree = EMPTY_LIST;
  C->working_tree = EMPTY_LIST;
  /* internal file names */
  C->file_names.circuit = NULL;
  C->file_names.param = NULL;
  C->file_names.passf = NULL;
  C->file_names.envelope = NULL;
  C->file_names.env_call = NULL;
  C->file_names.plot = NULL;
  C->file_names.iter = NULL;
  C->file_names.pname = NULL;
  /* node defaults */
  C->node_defaults.name = NULL;
  C->node_defaults.units = 'V';
  C->node_defaults.dt = 100e-12;
  C->node_defaults.dx = 1;
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
  C->extensions.env_call = ".env_call";
  C->extensions.plot = ".plot";
  /* pretty weak */
  C->extensions.which_trace = malloc(LINE_LENGTH);  // mem:intratubal
  /* options */
  C->options.binsearch_accuracy = 0.1;
  C->options.spice_call_name = strdup("wrspice");  // mem:descendentalism
  C->options.spice_verbose = 0;
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

/* Validates that this table contains no keys except for the ones passed.
 * Terminate the argument list with NULL.
 * Returns false if the table contains keys other than those passed as arguments. */
static bool keys_ok(Builder *C, toml_table_t *t, ...)
{
  for (size_t i = 0;; ++i) {
    const char *key = toml_key_in(t, i);
    if (!key) {
      break;
    }

    va_list argv;
    va_start(argv, t);
    for (;;) {
      char *arg = va_arg(argv, const char *);
      if (!arg) {
        // this key was not found in the arguments list
        break;
      } else if (strcmp(arg, key) == 0) {
        goto next_key;
      }
    }
    va_end(argv);

    warn("Unknown key: '%s'\n", key);
    return false;
  next_key:;
  }
  return true;
}

#define SCHEMA(table, ...)                        \
  toml_table_t *table = toml_table_in(t, #table); \
  if (!table) {                                   \
    return 0;                                     \
  }                                               \
  if (!keys_ok(C, table, __VA_ARGS__, NULL)) {    \
    error("While parsing [" #table "]\n");        \
  }

/* Parse the [simulator] options section of this file.
 * Returns 0 if the section is not present or incomplete and 1 otherwise. */
static int read_simulator(Builder *C, toml_table_t *t)
{
  SCHEMA(simulator, "max_subprocesses", "command", "verbose");
  int n = 0;
  n += read_an_int(&C->options.max_subprocesses, simulator, "max_subprocesses");
  n += read_a_string(&C->options.spice_call_name, simulator, "command");
  n += read_a_bool(&C->options.spice_verbose, simulator, "verbose");
  return n;
}

/* Parse the [envelope] section of this file.
 * Returns 0 if the section is not present or incomplete and 1 otherwise. */
static int read_envelope(Builder *C, toml_table_t *t)
{
  // read [envelope] table (NOTE: must be done before nodes)
  SCHEMA(envelope, "dx", "dt");
  return read_a_double(&C->node_defaults.dx, envelope, "dx") &&
         read_a_double(&C->node_defaults.dt, envelope, "dt");
}

/* Read the `nodes` list from the top level of the toml file. Also reads [envelope] if nodes are
 * present.
 * Returns the number of nodes successfully read. */
static int read_nodes(Builder *C, toml_table_t *t)
{
  int len_nodes = 0;
  C->has_envelope |= read_envelope(C, t);
  toml_array_t *nodes = toml_array_in(t, "nodes");
  if (nodes) {
    // nodes are present: [envelope] must be too, for defaults
    if (!C->has_envelope) {
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
          // if (!keys_ok(C, node, "node", "envelope", NULL)) {
          //  TODO: permit nodes to have metadata, and check that dx, dt are nonzero
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
  if (!parameters) {
    return 0;
  }
  // loop over the table entries
  int i;
  for (i = 0;; ++i) {
    const char *parameter = toml_key_in(parameters, i);
    if (!parameter)
      break;

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
      C->params = realloc(C->params, (++C->num_params_all) * sizeof *C->params);  // mem:restringer
      C->params[n] = PARAM_DEFAULT;
    }

    // populate/overwrite C->params[n] with the contents of `values`
    //free(C->params[n].name);
    C->params[n] = PARAM_DEFAULT;
    // parameter name:
    C->params[n].name = strdup(parameter);  // mem:reminiscer

    // nominal-only parameters are simply numeric, and never included in analysis
    toml_datum_t nominal = toml_double_in(parameters, parameter);
    if (nominal.ok) {
      C->params[n].nominal = nominal.u.d;
      C->params[n].include = false;
      // make maltspace happy
      C->params[n].logs = false;
      C->params[n].sigabs = 1.0;
      C->params[n].sigma = 0.0;
      continue;
    }

    // included parameters
    toml_table_t *values = toml_table_in(parameters, parameter);
    if (!values) {
      error("[parameters.%s] is not a table\n", parameter);
    }

    // nominal (required):
    if (!read_a_double(&C->params[n].nominal, values, "nominal")) {
      error("Parameter '%s' has no nominal value\n", parameter);
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

    // sigma, sigabs (exactly 1 required for included parameters):
    // TODO: parse a subtable {percent = 5} or whatever
    toml_datum_t sigma = toml_double_in(values, "sigma");
    toml_datum_t sigabs = toml_double_in(values, "sig_abs");
    if (sigma.ok && !sigabs.ok) {
      C->params[n].sigma = sigma.u.d;
      C->params[n].sigabs = 0.0;
    } else if (sigabs.ok && !sigma.ok) {
      C->params[n].sigabs = sigabs.u.d;
      C->params[n].sigma = 0.0;
    } else if (C->params[n].include) {
      error("Parameter '%s' must have exactly one of sigma or sig_abs\n", parameter);
      return 0;
    }
  }
  return i;
}

/* Reads the [extensions] table containing file extensions from the TOML file, if present.
 * Returns the number of extensions successfully read. */
static int read_extensions(Builder *C, toml_table_t *t)
{
  SCHEMA(extensions, "circuit", "plot", "parameters", "passfail", "envelope", "env_call");
  int n = 0;
  n += read_a_string(&C->extensions.circuit, extensions, "circuit");  // mem:timetaker
  n += read_a_string(&C->extensions.plot, extensions, "plot");        // mem:irrepairable
  n += read_a_string(&C->extensions.param, extensions, "parameters");
  n += read_a_string(&C->extensions.passf, extensions, "passfail");
  n += read_a_string(&C->extensions.envelope, extensions, "envelope");
  n += read_a_string(&C->extensions.env_call, extensions, "env_call");
  return n;
}

/* Reads the [define] table (define options) from the TOML file, if present.
 * Returns the number of key-value pairs successfully converted. */
static int read_define_opts(Builder *C, toml_table_t *t)
{
  SCHEMA(define, "simulate", "envelope");
  int n = 0;
  n += read_a_bool(&C->options.d_simulate, define, "simulate");
  n += read_a_bool(&C->options.d_envelope, define, "envelope");
  return n;
}

/* Reads the [yield] table (corners-yield options) from the TOML file, if present.
 * Returns the number of key-value pairs successfully converted. */
static int read_yield_opts(Builder *C, toml_table_t *t)
{
  SCHEMA(yield, "search_depth", "search_width", "search_steps", "max_mem_k", "accuracy",
         "print_every");
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
  SCHEMA(optimize, "min_iter", "max_mem_k");
  int n = 0;
  n += read_an_int(&C->options.o_min_iter, optimize, "min_iter");
  n += read_an_int(&C->options.o_min_iter, optimize, "max_mem_k");
  return n;
}

/* Reads the [xy] table (2D sweep settings) from the TOML file, if present.
 * Returns the number of sweeps successfully converted. */
static int read_xy_sweeps(Builder *C, toml_table_t *t)
{
  SCHEMA(xy, "sweeps", "iterations");
  toml_array_t *sweeps = toml_array_in(xy, "sweeps");
  int len_sweeps = 0;
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
  return len_sweeps;
}

/* If there is a file at *filename, open and parse it.
 * Returns 1 if the file was successfully parsed, 0 if the file does not exist; aborts otherwise. */
static int try_parse_configuration(Builder *C, char *filename)
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
  if (!keys_ok(C, t, "print_terminal", "binsearch_accuracy", "simulator", "nodes", "parameters",
               "envelope", "extensions", "define", "margins", "trace", "yield", "optimize", "xy",
               NULL)) {
    error("While parsing a TOML file (%s)\n", filename);
  }
  // TODO: check that print_terminal is working as intended
  read_a_bool(&C->options.print_terminal, t, "print_terminal");
  read_a_double(&C->options.binsearch_accuracy, t, "binsearch_accuracy");

  read_simulator(C, t);
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

/* Checks whether or not a file exists and is a regular file. */
static bool file_exists(const char *path)
{
  struct stat s;
  return stat(path, &s) == 0 && S_ISREG(s.st_mode);
}

/* Finds the most specific file with the extension `ext` prefixed by any element of `tree`.
 *
 * Returns NULL if no such file exists.
 */
static char *most_specific_with_ext(const list_t *tree, const char *ext)
{
  char *path = NULL;
  for (int i = tree->len; i-- > 0;) {
    // check path/to/foo.ext
    resprintf(&path, "%s%s", tree->ptr[i], ext);
    if (file_exists(path)) {
      return path;
    }
    // check path/to/foo/the.ext
    resprintf(&path, "%s/the%s", tree->ptr[i], ext);
    if (file_exists(path)) {
      return path;
    }
  }
  free(path);
  return NULL;
}

static const char *file_extension_by_type(const struct extensions *e, enum filetype ftype)
{
  switch (ftype) {
  case Ft_Circuit:
    return e->circuit;
  case Ft_Parameters:
    return e->param;
  case Ft_PassFail:
    return e->passf;
  case Ft_Envelope:
    return e->envelope;
  case Ft_EnvCall:
    return e->env_call;
  case Ft_Plot:
    return e->plot;
  default:
    fprintf(stderr, "Internal error (%d is not a file type)", ftype);
    exit(EXIT_FAILURE);
  }
}

/* Creates a NEW file of `ftype` in the immediate working directory, setting the appropriate entry
 * in C->file_types.
 *
 * Returns a pointer to the opened FILE, or aborts if fopen fails. */
FILE *new_file_by_type(Configuration *C, enum filetype ftype)
{
  const char *ext = file_extension_by_type(&C->extensions, ftype);

  char **filename;
  switch (ftype) {
  case Ft_Circuit:
    filename = &C->file_names.circuit;
    break;
  case Ft_Parameters:
    filename = &C->file_names.param;
    break;
  case Ft_PassFail:
    filename = &C->file_names.passf;
    break;
  case Ft_Envelope:
    filename = &C->file_names.envelope;
    break;
  case Ft_EnvCall:
    filename = &C->file_names.env_call;
    break;
  case Ft_Plot:
    filename = &C->file_names.plot;
    break;
  default:
    error("Internal error (%d is not a file type)", ftype);
  }

  // set the new filename, clobbering what was in file_names (if any)
  char *cwd = getcwd(NULL, 0);
  resprintf(filename, "%s/the%s", cwd, ext);
  free(cwd);

  // open the file for writing
  FILE *ptr = fopen(*filename, "w");
  if (ptr == NULL) {
    error("Can not open the %s file '%s'\n", ext, *filename);
  }
  return ptr;
}

/* Finds the most specific EXISTING file of `ftype` in either the project tree or the working tree,
 * returning its name or NULL if no such file exists.
 */
static char *find_file_by_type(const Builder *C, enum filetype ftype)
{
  const char *ext = file_extension_by_type(&C->extensions, ftype);
  char *path = most_specific_with_ext(&C->project_tree, ext);
  if (path == NULL) {
    path = most_specific_with_ext(&C->working_tree, ext);
  }
  return path;
}

/* Move data from the builder into the configuration. */
void build_configuration(Configuration *C, Builder *B)
{
  C->function = B->function;
  C->command = "";
  C->log = B->log;
  C->keep_files = B->keep_files;
  C->working_tree = B->working_tree;
  lst_drop(&B->project_tree);
  C->file_names = B->file_names;
  C->options = B->options;
  C->extensions = B->extensions;
  // drop default node & parameter data
  node_drop(&B->node_defaults);
  C->num_nodes = B->num_nodes;
  C->nodes = B->nodes;
  C->num_params_all = B->num_params_all;
  C->params = B->params;
  C->num_2D = B->num_2D;
  C->_2D = B->_2D;
  // remove excluded params from the running (initializing num_params_corn and num_params)
  exclude_params_corn(C);
}

/* Walks up the directory tree to find the directory where Malt.toml is located, and parses it.
 *
 * If no Malt.toml file is found, returns an empty list. Otherwise, returns the list of directory
 * names. Only the first entry is a valid path by itself; the rest are components that have to be
 * joined together with / to make a valid path.
 */
static list_t configure_project_root(Builder *C)
{
  list_t tree = EMPTY_LIST;

  char *current = getcwd(NULL, 0);  // mem:busybody
  char *filename = NULL;
  // in each path,
  for (;;) {
    // check if Malt.toml exists
    resprintf(&filename, "%s/Malt.toml", current);  // mem:finicky
    if (file_exists(filename)) {
      info("Found Malt.toml in '%s'\n", current);
      if (try_parse_configuration(C, filename)) {
        info("Parsed '%s'\n", filename);
      } else {
        // this shouldn't happen
        error("Failed to parse Malt.toml\n");
      }
      // add the current path to the list
      lst_push(&tree, current);
      // and reverse it to put it in correct order
      lst_reverse(&tree);
      goto found;
    }
    // if no Malt.toml, chop off the last component of the current path
    char *last_sep = strrchr(current, '/');
    *last_sep = '\0';
    if (!last_sep || last_sep == current) {
      // reached / without finding Malt.toml
      break;
    }
    // add this component to the list
    lst_push(&tree, strdup(last_sep + 1));  // mem:embrace
  }
  // Malt.toml not found: clean up temporary data
  lst_drop(&tree);
  free(current);  // mem:busybody
found:
  free(filename);  // mem:finicky
  return tree;
}

static void configure_directory(Builder *C, const char *next_component)
{
  char *path = NULL;
  // add the path itself to the project tree
  resprintf(&path, "%s/%s", lst_last(&C->project_tree), next_component);
  lst_push(&C->project_tree, path);

  // in each directory try both <next_component>.toml and <next_component>/the.toml
  char *filename = NULL;
  resprintf(&filename, "%s.toml", path);
  if (try_parse_configuration(C, filename)) {
    info("Parsed configuration: '%s'\n", filename);
  }
  resprintf(&filename, "%s/the.toml", path);
  if (try_parse_configuration(C, filename)) {
    info("Parsed configuration: '%s'\n", filename);
  }
  free(filename);

  // and create the parallel working directory, if it does not yet exist
  char *work = NULL;
  resprintf(&work, "%s/%s", lst_last(&C->working_tree), next_component);
  mkdir(work, 0777);
  lst_push(&C->working_tree, work);
}

/* Walks *down* the directory tree from the project root to the `target`, which is presumed to be
 * relative to the current directory, parsing .toml files whose names correspond to the next
 * component of the path. See `Configure` for more details.
 *
 * `ptree` should initially contain the path components of the project root, as if filled by
 * `configure_project_root`.
 */
static void configure_target(Builder *C, list_t ptree, char *target)
{
  assert(ptree.len > 0);

  lst_push(&C->project_tree, strdup(ptree.ptr[0]));
  // TODO: read working directory name from Malt.toml
  char *work = resprintf(NULL, "%s/%s", ptree.ptr[0], "_malt");
  lst_push(&C->working_tree, work);
  mkdir(work, 0777);

  // 1. traverse the directories already discovered by `configure_project_root`
  for (size_t i = 1; i < ptree.len; ++i) {
    char *component = ptree.ptr[i];
    configure_directory(C, component);
  }

  // 1.5. Validate `target`
  if (target[0] == '/') {
    error("Config path (%s) may not be absolute\n", target);
  }
  // chop off the trailing .toml or .cir of *target, if present
  char *ext = strrchr(target, '.');
  if (ext != NULL && (strcmp(ext, ".toml") == 0 || strcmp(ext, C->extensions.circuit) == 0)) {
    *ext = '\0';
  }
  // and one optional trailing slash
  char *trailing_slash = strrchr(target, '/');
  if (trailing_slash && trailing_slash[1] == '\0') {
    *trailing_slash = '\0';
  }

  // 2. traverse into `target` one path component at a time
  for (char *component = NULL, *etc = target; component != etc;) {
    component = etc;
    char *endp = strchr(etc, '/');
    if (endp != NULL) {
      *endp = '\0';
      etc = endp + 1;
    }
    if (strcmp(component, ".") == 0) {
      continue;
    }
    if (strcmp(component, "..") == 0) {
      // we've already processed the config file so can't back out
      error("Config path must not contain parent links (..)");
    }
    configure_directory(C, component);
  }

  lst_drop(&ptree);
}

/* Finds and parses all applicable configuration files.
 *
 * There are two kinds of configuration files that may apply:
 * - Exactly one file named Malt.toml, which may be found in the current directory or any ancestor
 *   up to the user's home directory or /.
 * - Every <config>.toml where /path/to/<config> is a prefix of the configuration path named on the
 *   command line, and inside the directory where Malt.toml is found.
 *
 * For instance, given the following directory structure:
 *     project/
 *     +- Malt.toml
 *     +- circuitname.toml
 *     +- circuitname/
 *     |  +- corners.toml
 *     |  +- corners/
 *     |  |  +- 10.toml
 *     |  |  +- 5.toml
 *
 * The command `malt -m circuitname/corners/10` (the .toml extension is optional) run inside the
 * project directory will read in this order:
 *   1. Malt.toml
 *   2. circuitname.toml
 *   3. circuitname/corners.toml
 *   4. circuitname/corners/10.toml
 */
Configuration *Configure(const Args *args, FILE *log)
{
  Builder B;
  Builder *C = &B;  // so we can use info, warn, error macros which assume a pointer

  builder_init(&B, args, log);

  // 1. Find Malt.toml and parse that
  list_t ptree = configure_project_root(&B);

  // If Malt.toml is missing, helpfully create a new one in .
  if (lst_empty(&ptree)) {
    warn("No Malt.toml configuration files found\n");
    FILE *fp = fopen("Malt.toml", "w");
    if (fp != NULL) {
      info("Generating a default configuration file './Malt.toml'\n");
    } else {
      error("Cannot open './Malt.toml' for writing\n");
    }
    builder_debug(&B, fp);
    fclose(fp);
  }

  // 2. Parse all other .toml files between project root and target
  configure_target(&B, ptree, args->configuration);

  // 3. Redirect log output to the working directory
  // (This is the earliest point we can do this because the working directory is not known
  // before calling `configure_target`)
  char *working_dir = lst_last(&B.working_tree);
  char *logname = resprintf(NULL, "%s/%c.out", working_dir, B.function);
  FILE *new = fopen(logname, "w");
  if (new == NULL) {
    warn("Cannot open file '%s'; log will be saved in a temporary file\n", logname);
  } else {
    // copy contents of the old temporary file into the new file
    rewind(B.log);
    for (;;) {
      int c = fgetc(B.log);
      if (c == EOF)
        break;
      fputc(c, new);
    }
    // and pull the old switcharoo
    fclose(B.log);  // (should delete the temporary file)
    B.log = new;
  }
  free(logname);

  // 4. Find input filenames {circuit, param, envelope, passfail}
  B.file_names.circuit = most_specific_with_ext(&B.project_tree, B.extensions.circuit);
  B.file_names.param = find_file_by_type(&B, Ft_Parameters);
  B.file_names.passf = find_file_by_type(&B, Ft_PassFail);
  B.file_names.envelope = find_file_by_type(&B, Ft_Envelope);
  B.file_names.env_call = find_file_by_type(&B, Ft_EnvCall);

  // 5. Write config to output TOML file
  char *filename = resprintf(NULL, "%s/%c.toml", working_dir, B.function);
  FILE *fp = fopen(filename, "w");
  if (fp != NULL) {
    builder_debug(&B, fp);
    fclose(fp);
    info("Configuration written to '%s'\n", filename);
  } else {
    warn("Cannot open '%s' for writing\n", filename);
  }
  free(filename);

  Configuration *cfg = malloc(sizeof *cfg);  // mem:circumgyrate
  build_configuration(cfg, &B);

  return cfg;
}

/* Unlink C->file_names.pname, unless -k was specified on the command line. */
void unlink_pname(Configuration *C)
{
  if (!C->keep_files) {
    unlink(C->file_names.pname);
  }
}
