/*
 * src/bin/pgcopydb/cli_common.c
 *     Implementation of a CLI which lets you run individual keeper routines
 *     directly
 *
 * Copyright (c) Microsoft Corporation. All rights reserved.
 * Licensed under the PostgreSQL License.
 *
 */
#include <errno.h>
#include <inttypes.h>
#include <getopt.h>
#include <signal.h>

#include "commandline.h"

#include "cli_common.h"
#include "cli_root.h"
#include "commandline.h"
#include "copydb.h"
#include "env_utils.h"
#include "file_utils.h"
#include "log.h"
#include "parsing_utils.h"
#include "string_utils.h"

/* handle command line options for our setup. */
CopyDBOptions copyDBoptions = { 0 };
bool outputJSON = false;


/*
 * Provide help.
 */
void
cli_help(int argc, char **argv)
{
	CommandLine command = root;

	if (env_exists(PGCOPYDB_DEBUG))
	{
		command = root_with_debug;
	}

	(void) commandline_print_command_tree(&command, stdout);
}


/*
 * cli_print_version_getopts parses the CLI options for the pgcopydb version
 * command, which are the usual suspects.
 */
int
cli_print_version_getopts(int argc, char **argv)
{
	int c, option_index = 0;

	static struct option long_options[] = {
		{ "json", no_argument, NULL, 'J' },
		{ "version", no_argument, NULL, 'V' },
		{ "verbose", no_argument, NULL, 'v' },
		{ "notice", no_argument, NULL, 'v' },
		{ "debug", no_argument, NULL, 'd' },
		{ "trace", no_argument, NULL, 'z' },
		{ "quiet", no_argument, NULL, 'q' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 }
	};
	optind = 0;

	/*
	 * The only command lines that are using keeper_cli_getopt_pgdata are
	 * terminal ones: they don't accept subcommands. In that case our option
	 * parsing can happen in any order and we don't need getopt_long to behave
	 * in a POSIXLY_CORRECT way.
	 *
	 * The unsetenv() call allows getopt_long() to reorder arguments for us.
	 */
	unsetenv("POSIXLY_CORRECT");

	while ((c = getopt_long(argc, argv, "JVvdzqh",
							long_options, &option_index)) != -1)
	{
		switch (c)
		{
			case 'J':
			{
				outputJSON = true;
				log_trace("--json");
				break;
			}

			case 'h':
			{
				commandline_help(stderr);
				exit(EXIT_CODE_QUIT);
				break;
			}

			default:
			{
				/*
				 * Ignore errors, ignore most of the things, just print the
				 * version and exit(0)
				 */
				break;
			}
		}
	}
	return optind;
}


/*
 * keeper_cli_print_version prints the pgcopydb version and exits with
 * successful exit code of zero.
 */
void
cli_print_version(int argc, char **argv)
{
	if (outputJSON)
	{
		JSON_Value *js = json_value_init_object();
		JSON_Object *root = json_value_get_object(js);

		json_object_set_string(root, "pgcopydb", VERSION_STRING);
		json_object_set_string(root, "pg_major", PG_MAJORVERSION);
		json_object_set_string(root, "pg_version", PG_VERSION);
		json_object_set_string(root, "pg_version_str", PG_VERSION_STR);
		json_object_set_number(root, "pg_version_num", (double) PG_VERSION_NUM);

		(void) cli_pprint_json(js);
	}
	else
	{
		fformat(stdout, "pgcopydb version %s\n", VERSION_STRING);
		fformat(stdout, "compiled with %s\n", PG_VERSION_STR);
		fformat(stdout, "compatible with Postgres 10, 11, 12, 13, 14, and 15\n");
	}

	exit(0);
}


/*
 * cli_pprint_json pretty prints the given JSON value to stdout and frees the
 * JSON related memory.
 */
void
cli_pprint_json(JSON_Value *js)
{
	/* output our nice JSON object, pretty printed please */
	char *serialized_string = json_serialize_to_string_pretty(js);
	fformat(stdout, "%s\n", serialized_string);

	/* free intermediate memory */
	json_free_serialized_string(serialized_string);
	json_value_free(js);
}


/*
 * cli_copydb_getenv reads from the environment variables and fills-in the
 * command line options.
 */
bool
cli_copydb_getenv(CopyDBOptions *options)
{
	int errors = 0;

	/* now some of the options can be also set from the environment */
	if (env_exists(PGCOPYDB_SOURCE_PGURI))
	{
		if (!get_env_copy(PGCOPYDB_SOURCE_PGURI,
						  options->source_pguri,
						  sizeof(options->source_pguri)))
		{
			/* errors have already been logged */
			++errors;
		}
	}

	if (env_exists(PGCOPYDB_TARGET_PGURI))
	{
		if (!get_env_copy(PGCOPYDB_TARGET_PGURI,
						  options->target_pguri,
						  sizeof(options->target_pguri)))
		{
			/* errors have already been logged */
			++errors;
		}
	}

	if (env_exists(PGCOPYDB_TABLE_JOBS))
	{
		char jobs[BUFSIZE] = { 0 };

		if (get_env_copy(PGCOPYDB_TABLE_JOBS, jobs, sizeof(jobs)))
		{
			if (!stringToInt(jobs, &options->tableJobs) ||
				options->tableJobs < 1 ||
				options->tableJobs > 128)
			{
				log_fatal("Failed to parse PGCOPYDB_TABLE_JOBS: \"%s\"",
						  jobs);
				++errors;
			}
		}
		else
		{
			/* errors have already been logged */
			++errors;
		}
	}

	if (env_exists(PGCOPYDB_INDEX_JOBS))
	{
		char jobs[BUFSIZE] = { 0 };

		if (get_env_copy(PGCOPYDB_INDEX_JOBS, jobs, sizeof(jobs)))
		{
			if (!stringToInt(jobs, &options->indexJobs) ||
				options->indexJobs < 1 ||
				options->indexJobs > 128)
			{
				log_fatal("Failed to parse PGCOPYDB_INDEX_JOBS: \"%s\"",
						  jobs);
				++errors;
			}
		}
		else
		{
			/* errors have already been logged */
			++errors;
		}
	}

	if (env_exists(PGCOPYDB_SPLIT_TABLES_LARGER_THAN))
	{
		char bytes[BUFSIZE] = { 0 };

		if (get_env_copy(PGCOPYDB_SPLIT_TABLES_LARGER_THAN, bytes, sizeof(bytes)))
		{
			if (!cli_parse_bytes_pretty(
					bytes,
					&options->splitTablesLargerThan,
					(char *) &options->splitTablesLargerThanPretty,
					sizeof(options->splitTablesLargerThanPretty)))
			{
				log_fatal("Failed to parse PGCOPYDB_SPLIT_TABLES_LARGER_THAN: "
						  " \"%s\"",
						  bytes);
				++errors;
			}
		}
		else
		{
			/* errors have already been logged */
			++errors;
		}
	}

	/* when --snapshot has not been used, check PGCOPYDB_SNAPSHOT */
	if (env_exists(PGCOPYDB_SNAPSHOT))
	{
		if (!get_env_copy(PGCOPYDB_SNAPSHOT,
						  options->snapshot,
						  sizeof(options->snapshot)))
		{
			/* errors have already been logged */
			++errors;
		}
	}

	/* check --plugin environment variable */
	if (env_exists(PGCOPYDB_OUTPUT_PLUGIN))
	{
		char plugin[BUFSIZE] = { 0 };

		if (!get_env_copy(PGCOPYDB_OUTPUT_PLUGIN, plugin, BUFSIZE))
		{
			/* errors have already been logged */
			++errors;
		}

		options->slot.plugin = OutputPluginFromString(plugin);

		if (options->slot.plugin == STREAM_PLUGIN_UNKNOWN)
		{
			log_fatal("Unknown replication plugin \"%s\", please use either "
					  "test_decoding (the default) or wal2json",
					  OutputPluginToString(options->slot.plugin));
			++errors;
		}
	}

	/* when --drop-if-exists has not been used, check PGCOPYDB_DROP_IF_EXISTS */
	if (!options->restoreOptions.dropIfExists)
	{
		if (env_exists(PGCOPYDB_DROP_IF_EXISTS))
		{
			char DROP_IF_EXISTS[BUFSIZE] = { 0 };

			if (!get_env_copy(PGCOPYDB_DROP_IF_EXISTS,
							  DROP_IF_EXISTS,
							  sizeof(DROP_IF_EXISTS)))
			{
				/* errors have already been logged */
				++errors;
			}
			else if (!parse_bool(DROP_IF_EXISTS,
								 &(options->restoreOptions.dropIfExists)))
			{
				log_error("Failed to parse environment variable \"%s\" "
						  "value \"%s\", expected a boolean (on/off)",
						  PGCOPYDB_DROP_IF_EXISTS,
						  DROP_IF_EXISTS);
				++errors;
			}
		}
	}

	/* when --fail-fast has not been used, check PGCOPYDB_FAIL_FAST */
	if (!options->failFast)
	{
		if (env_exists(PGCOPYDB_FAIL_FAST))
		{
			char FAIL_FAST[BUFSIZE] = { 0 };

			if (!get_env_copy(PGCOPYDB_FAIL_FAST, FAIL_FAST, sizeof(FAIL_FAST)))
			{
				/* errors have already been logged */
				++errors;
			}
			else if (!parse_bool(FAIL_FAST, &(options->failFast)))
			{
				log_error("Failed to parse environment variable \"%s\" "
						  "value \"%s\", expected a boolean (on/off)",
						  PGCOPYDB_FAIL_FAST,
						  FAIL_FAST);
				++errors;
			}
		}
	}

	/* when --fail-fast has not been used, check PGCOPYDB_FAIL_FAST */
	if (!options->skipVacuum)
	{
		if (env_exists(PGCOPYDB_SKIP_VACUUM))
		{
			char SKIP_VACUUM[BUFSIZE] = { 0 };

			if (!get_env_copy(PGCOPYDB_SKIP_VACUUM,
							  SKIP_VACUUM,
							  sizeof(SKIP_VACUUM)))
			{
				/* errors have already been logged */
				++errors;
			}
			else if (!parse_bool(SKIP_VACUUM, &(options->skipVacuum)))
			{
				log_error("Failed to parse environment variable \"%s\" "
						  "value \"%s\", expected a boolean (on/off)",
						  PGCOPYDB_SKIP_VACUUM,
						  SKIP_VACUUM);
				++errors;
			}
		}
	}

	return errors == 0;
}


/*
 * cli_copydb_is_consistent returns false when the option --not-consistent
 * should be used.
 */
bool
cli_copydb_is_consistent(CopyDBOptions *options)
{
	CopyFilePaths cfPaths = { 0 };
	char *dir = IS_EMPTY_STRING_BUFFER(options->dir) ? NULL : options->dir;

	if (!copydb_prepare_filepaths(&cfPaths, dir, false))
	{
		return false;
	}

	/*
	 * Read the snapshot, origin, plugin, and slot-name files from the previous
	 * command or run, unless --restart is explicitely provided.
	 */
	if (!cli_read_previous_options(options, &cfPaths))
	{
		/* errors have already been logged */
		return false;
	}

	/* when --resume is not used, we're good */
	if (!options->resume)
	{
		return true;
	}

	/* when --resume and --not-consistent are used, we're good */
	if (options->resume && options->notConsistent)
	{
		return true;
	}

	/*
	 * Here --resume is used and we're expected to be consisten with the
	 * previous pgcopydb run/attempt/command. That requires re-using a
	 * snapshot.
	 */
	if (IS_EMPTY_STRING_BUFFER(options->snapshot))
	{
		/* --resume without --snapshot requires --not-consistent */
		log_error("Options --snapshot is mandatory unless using --not-consistent");
		return false;
	}

	return true;
}


/*
 * cli_read_previous_options reads the options that have been set on a previous
 * command such as pgcopydb snapshot or pgcopydb stream setup.
 */
bool
cli_read_previous_options(CopyDBOptions *options, CopyFilePaths *cfPaths)
{
	struct optFromFile
	{
		char *filename;
		char *optname;
		char *varname;
		char *def;
		char *target;
		size_t size;
	}
	opts[] =
	{
		{
			cfPaths->snfile,
			"--snapshot",
			"snapshot",
			NULL,
			options->snapshot,
			sizeof(options->snapshot)
		},
		{
			cfPaths->cdc.originfile,
			"--origin",
			"origin",
			REPLICATION_ORIGIN,
			options->origin,
			sizeof(options->origin)
		}
	};

	int count = sizeof(opts) / sizeof(opts[0]);

	for (int i = 0; i < count; i++)
	{
		/* bypass non-existing files, just use the command line options then */
		if (options->restart || !file_exists(opts[i].filename))
		{
			/* install default value if needed */
			if (opts[i].def != NULL && IS_EMPTY_STRING_BUFFER(opts[i].target))
			{
				strlcpy(opts[i].target, opts[i].def, opts[i].size);
			}

			continue;
		}

		/* allocate an intermediate value to read from file */
		char *val = (char *) calloc(opts[i].size, sizeof(char));

		if (!cli_read_one_line(opts[i].filename,
							   opts[i].varname,
							   val,
							   opts[i].size))
		{
			/* errors have already been logged */
			return false;
		}

		/* if the command line --option has not been used, use val */
		if (IS_EMPTY_STRING_BUFFER(opts[i].target))
		{
			strlcpy(opts[i].target, val, opts[i].size);

			log_notice("Re-using %s '%s' found at \"%s\"",
					   opts[i].optname,
					   opts[i].target,
					   opts[i].filename);
		}

		/*
		 * Otherwise make sure on-file and command line use the same value,
		 * unless --not-consistent is used, which allows for using new ones.
		 */
		else if (!options->notConsistent && !streq(opts[i].target, val))
		{
			log_error("Failed to ensure consistency of %s", opts[i].optname);
			log_error("Previous run was done with %s \"%s\" and current run "
					  "is using %s \"%s\"",
					  opts[i].varname,
					  val,
					  opts[i].optname,
					  opts[i].target);
			return false;
		}
	}

	/*
	 * Now read the replication slot file, which includes information for both
	 * --slot-name and --plugin option, and more.
	 */
	if (options->restart || !file_exists(cfPaths->cdc.slotfile))
	{
		/*
		 * Only install a default value for the --plugin option when it wasn't
		 * previously set from an environment variable or another way.
		 */
		if (IS_EMPTY_STRING_BUFFER(options->slot.slotName))
		{
			strlcpy(options->slot.slotName, REPLICATION_SLOT_NAME,
					sizeof(options->slot.slotName));
		}

		if (options->slot.plugin == STREAM_PLUGIN_UNKNOWN)
		{
			options->slot.plugin = OutputPluginFromString(REPLICATION_PLUGIN);
		}
	}
	else
	{
		ReplicationSlot onFileSlot = { 0 };

		if (!snapshot_read_slot(cfPaths->cdc.slotfile, &onFileSlot))
		{
			/* errors have already been logged */
			return false;
		}

		if (!IS_EMPTY_STRING_BUFFER(options->slot.slotName) &&
			!streq(options->slot.slotName, onFileSlot.slotName))
		{
			log_error("Failed to ensure consistency of --slot-name");
			log_error("Previous run was done with slot-name \"%s\" and "
					  "current run is using --slot-name \"%s\"",
					  onFileSlot.slotName,
					  options->slot.slotName);
			return false;
		}

		if (options->slot.plugin != STREAM_PLUGIN_UNKNOWN &&
			options->slot.plugin != onFileSlot.plugin)
		{
			log_error("Failed to ensure consistency of --plugin");
			log_error("Previous run was done with plugin \"%s\" and "
					  "current run is using --plugin \"%s\"",
					  OutputPluginToString(onFileSlot.plugin),
					  OutputPluginToString(options->slot.plugin));
			return false;
		}

		/* copy the onFileSlot over to our options, wholesale */
		options->slot = onFileSlot;
	}

	if (options->slot.plugin == STREAM_PLUGIN_UNKNOWN)
	{
		log_fatal("Unknown replication plugin \"%s\", please use either "
				  "test_decoding (the default) or wal2json",
				  OutputPluginToString(options->slot.plugin));
		return false;
	}

	return true;
}


/*
 * cli_read_one_line reads a file with a single line and place the contents of
 * that line into the given string buffer.
 */
bool
cli_read_one_line(const char *filename,
				  const char *name,
				  char *target,
				  size_t size)
{
	char *contents = NULL;
	long fileSize = 0L;

	if (!read_file(filename, &contents, &fileSize))
	{
		/* errors have already been logged */
		return false;
	}

	/* make sure to use only the first line of the file, without \n */
	char *lines[BUFSIZE] = { 0 };
	int lineCount = splitLines(contents, lines, BUFSIZE);

	if (lineCount != 1)
	{
		log_error("Failed to parse %s file \"%s\"", name, filename);
		free(contents);
		return false;
	}

	if (size < (strlen(lines[0]) + 1))
	{
		log_error("Failed to parse %s \"%s\" with %lld bytes, "
				  "pgcopydb supports only snapshot references up to %lld bytes",
				  name,
				  lines[0],
				  (long long) strlen(lines[0]) + 1,
				  (long long) size);
		free(contents);
		return false;
	}

	/* publish the one line to the snapshot variable */
	strlcpy(target, lines[0], size);
	free(contents);

	return true;
}


/*
 * cli_copy_db_getopts parses the CLI options for the `copy db` command.
 */
int
cli_copy_db_getopts(int argc, char **argv)
{
	CopyDBOptions options = { 0 };
	int c, option_index = 0, errors = 0;
	int verboseCount = 0;

	static struct option long_options[] = {
		{ "source", required_argument, NULL, 'S' },
		{ "target", required_argument, NULL, 'T' },
		{ "dir", required_argument, NULL, 'D' },
		{ "jobs", required_argument, NULL, 'J' },
		{ "table-jobs", required_argument, NULL, 'J' },
		{ "index-jobs", required_argument, NULL, 'I' },
		{ "split-tables-larger-than", required_argument, NULL, 'L' },
		{ "split-at", required_argument, NULL, 'L' },
		{ "drop-if-exists", no_argument, NULL, 'c' }, /* pg_restore -c */
		{ "roles", no_argument, NULL, 'A' },          /* pg_dumpall --roles-only */
		{ "no-role-passwords", no_argument, NULL, 'P' },
		{ "no-owner", no_argument, NULL, 'O' },       /* pg_restore -O */
		{ "no-comments", no_argument, NULL, 'X' },
		{ "no-acl", no_argument, NULL, 'x' }, /* pg_restore -x */
		{ "skip-blobs", no_argument, NULL, 'B' },
		{ "skip-large-objects", no_argument, NULL, 'B' },
		{ "skip-extensions", no_argument, NULL, 'e' },
		{ "skip-collations", no_argument, NULL, 'l' },
		{ "skip-vacuum", no_argument, NULL, 'U' },
		{ "filter", required_argument, NULL, 'F' },
		{ "filters", required_argument, NULL, 'F' },
		{ "ddl-dir", required_argument, NULL, 'y' },
		{ "fail-fast", no_argument, NULL, 'i' },
		{ "restart", no_argument, NULL, 'r' },
		{ "resume", no_argument, NULL, 'R' },
		{ "not-consistent", no_argument, NULL, 'C' },
		{ "snapshot", required_argument, NULL, 'N' },
		{ "follow", no_argument, NULL, 'f' },
		{ "plugin", required_argument, NULL, 'p' },
		{ "slot-name", required_argument, NULL, 's' },
		{ "origin", required_argument, NULL, 'o' },
		{ "create-slot", no_argument, NULL, 't' },
		{ "endpos", required_argument, NULL, 'E' },
		{ "version", no_argument, NULL, 'V' },
		{ "verbose", no_argument, NULL, 'v' },
		{ "notice", no_argument, NULL, 'v' },
		{ "debug", no_argument, NULL, 'd' },
		{ "trace", no_argument, NULL, 'z' },
		{ "quiet", no_argument, NULL, 'q' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 }
	};

	optind = 0;

	/* install default values */
	options.tableJobs = DEFAULT_TABLE_JOBS;
	options.indexJobs = DEFAULT_INDEX_JOBS;
	options.splitTablesLargerThan = DEFAULT_SPLIT_TABLES_LARGER_THAN;

	/* read values from the environment */
	if (!cli_copydb_getenv(&options))
	{
		log_fatal("Failed to read default values from the environment");
		exit(EXIT_CODE_BAD_ARGS);
	}

	while ((c = getopt_long(argc, argv,
							"S:T:D:J:I:L:cOBelirRCN:xXCtfo:p:s:E:F:y:iVvdzqh",
							long_options, &option_index)) != -1)
	{
		switch (c)
		{
			case 'S':
			{
				if (!validate_connection_string(optarg))
				{
					log_fatal("Failed to parse --source connection string, "
							  "see above for details.");
					++errors;
				}
				strlcpy(options.source_pguri, optarg, MAXCONNINFO);
				log_trace("--source %s", options.source_pguri);
				break;
			}

			case 'T':
			{
				if (!validate_connection_string(optarg))
				{
					log_fatal("Failed to parse --target connection string, "
							  "see above for details.");
					++errors;
				}
				strlcpy(options.target_pguri, optarg, MAXCONNINFO);
				log_trace("--target %s", options.target_pguri);
				break;
			}

			case 'D':
			{
				strlcpy(options.dir, optarg, MAXPGPATH);
				log_trace("--dir %s", options.dir);
				break;
			}

			case 'J':
			{
				if (!stringToInt(optarg, &options.tableJobs) ||
					options.tableJobs < 1 ||
					options.tableJobs > 128)
				{
					log_fatal("Failed to parse --jobs count: \"%s\"", optarg);
					++errors;
				}
				log_trace("--table-jobs %d", options.tableJobs);
				break;
			}

			case 'I':
			{
				if (!stringToInt(optarg, &options.indexJobs) ||
					options.indexJobs < 1 ||
					options.indexJobs > 128)
				{
					log_fatal("Failed to parse --index-jobs count: \"%s\"", optarg);
					++errors;
				}
				log_trace("--jobs %d", options.indexJobs);
				break;
			}

			case 'L':
			{
				if (!cli_parse_bytes_pretty(
						optarg,
						&options.splitTablesLargerThan,
						(char *) &options.splitTablesLargerThanPretty,
						sizeof(options.splitTablesLargerThanPretty)))
				{
					log_fatal("Failed to parse --split-tables-larger-than: \"%s\"",
							  optarg);
					++errors;
				}

				log_trace("--split-tables-larger-than %s (%lld)",
						  options.splitTablesLargerThanPretty,
						  (long long) options.splitTablesLargerThan);
				break;
			}

			case 'c':
			{
				options.restoreOptions.dropIfExists = true;
				log_trace("--drop-if-exists");
				break;
			}

			case 'A':
			{
				options.roles = true;
				log_trace("--roles");
				break;
			}

			case 'P':
			{
				options.noRolesPasswords = true;
				log_trace("--no-role-passwords");
				break;
			}

			case 'O':
			{
				options.restoreOptions.noOwner = true;
				log_trace("--no-owner");
				break;
			}

			case 'x':
			{
				options.restoreOptions.noACL = true;
				log_trace("--no-ack");
				break;
			}

			case 'X':
			{
				options.restoreOptions.noComments = true;
				log_trace("--no-comments");
				break;
			}

			case 'B':
			{
				options.skipLargeObjects = true;
				log_trace("--skip-large-objects");
				break;
			}

			case 'e':
			{
				options.skipExtensions = true;
				log_trace("--skip-extensions");
				break;
			}

			case 'l':
			{
				options.skipCollations = true;
				log_trace("--skip-collations");
				break;
			}

			case 'U':
			{
				options.skipVacuum = true;
				log_trace("--skip-vacuum");
				break;
			}

			case 'i':
			{
				options.failFast = true;
				log_trace("--fail-fast");
				break;
			}

			case 'r':
			{
				options.restart = true;
				log_trace("--restart");

				if (options.resume)
				{
					log_fatal("Options --resume and --restart are not compatible");
				}
				break;
			}

			case 'R':
			{
				options.resume = true;
				log_trace("--resume");

				if (options.restart)
				{
					log_fatal("Options --resume and --restart are not compatible");
				}
				break;
			}

			case 'C':
			{
				options.notConsistent = true;
				log_trace("--not-consistent");
				break;
			}

			case 'N':
			{
				strlcpy(options.snapshot, optarg, sizeof(options.snapshot));
				log_trace("--snapshot %s", options.snapshot);
				break;
			}

			case 's':
			{
				strlcpy(options.slot.slotName, optarg, NAMEDATALEN);
				log_trace("--slot-name %s", options.slot.slotName);
				break;
			}

			case 'p':
			{
				options.slot.plugin = OutputPluginFromString(optarg);
				log_trace("--plugin %s",
						  OutputPluginToString(options.slot.plugin));
				break;
			}

			case 'o':
			{
				strlcpy(options.origin, optarg, NAMEDATALEN);
				log_trace("--origin %s", options.origin);
				break;
			}

			case 't':
			{
				options.createSlot = true;
				log_trace("--create-slot");
				break;
			}

			case 'f':
			{
				options.follow = true;
				log_trace("--follow");
				break;
			}

			case 'E':
			{
				if (!parseLSN(optarg, &(options.endpos)))
				{
					log_fatal("Failed to parse endpos LSN: \"%s\"", optarg);
					++errors;
				}

				log_trace("--endpos %X/%X",
						  (uint32_t) (options.endpos >> 32),
						  (uint32_t) options.endpos);
				break;
			}

			case 'F':
			{
				strlcpy(options.filterFileName, optarg, MAXPGPATH);
				log_trace("--filters \"%s\"", options.filterFileName);

				if (!file_exists(options.filterFileName))
				{
					log_error("Filters file \"%s\" does not exists",
							  options.filterFileName);
					++errors;
				}
				break;
			}

			case 'y':
			{
				strlcpy(options.ddldir, optarg, MAXPGPATH);
				log_trace("--dir %s", options.ddldir);
				break;
			}

			case 'V':
			{
				/* keeper_cli_print_version prints version and exits. */
				cli_print_version(argc, argv);
				break;
			}

			case 'v':
			{
				++verboseCount;
				switch (verboseCount)
				{
					case 1:
					{
						log_set_level(LOG_NOTICE);
						break;
					}

					case 2:
					{
						log_set_level(LOG_SQL);
						break;
					}

					case 3:
					{
						log_set_level(LOG_DEBUG);
						break;
					}

					default:
					{
						log_set_level(LOG_TRACE);
						break;
					}
				}
				break;
			}

			case 'd':
			{
				verboseCount = 3;
				log_set_level(LOG_DEBUG);
				break;
			}

			case 'z':
			{
				verboseCount = 4;
				log_set_level(LOG_TRACE);
				break;
			}

			case 'q':
			{
				log_set_level(LOG_ERROR);
				break;
			}

			case 'h':
			{
				commandline_help(stderr);
				exit(EXIT_CODE_QUIT);
				break;
			}

			case '?':
			{
				commandline_help(stderr);
				exit(EXIT_CODE_BAD_ARGS);
				break;
			}
		}
	}

	if (IS_EMPTY_STRING_BUFFER(options.source_pguri) ||
		IS_EMPTY_STRING_BUFFER(options.target_pguri))
	{
		log_fatal("Options --source and --target are mandatory");
		exit(EXIT_CODE_BAD_ARGS);
	}

	if (!cli_copydb_is_consistent(&options))
	{
		log_fatal("Option --resume requires option --not-consistent");
		exit(EXIT_CODE_BAD_ARGS);
	}

	if (errors > 0)
	{
		commandline_help(stderr);
		exit(EXIT_CODE_BAD_ARGS);
	}

	/* publish our option parsing in the global variable */
	copyDBoptions = options;

	return optind;
}


/*
 * cli_parse_bytes_pretty parses a pretty-printed bytes value in the bytestring
 * argument, and converts it to a raw bytes value. Then it pretty-prints the
 * raw value in the bytesPretty allocated string, using pgcopydb rules.
 */
bool
cli_parse_bytes_pretty(const char *byteString,
					   uint64_t *bytes,
					   char *bytesPretty,
					   size_t bytesPrettySize)
{
	if (!parse_pretty_printed_bytes(byteString, bytes))
	{
		/* errors have already been logged */
		return false;
	}

	/* "1024 MB" will then be written as "1 GB" */
	(void) pretty_print_bytes(bytesPretty, bytesPrettySize, *bytes);

	log_trace("parsed bytes value: %lld", (long long) *bytes);
	log_trace("pretty printed to : \"%s\"", bytesPretty);

	return true;
}


/*
 * cli_copy_prepare_specs initializes our internal data structure that are used
 * to drive the operations.
 */
void
cli_copy_prepare_specs(CopyDataSpec *copySpecs, CopyDataSection section)
{
	PostgresPaths *pgPaths = &(copySpecs->pgPaths);

	char scrubbedSourceURI[MAXCONNINFO] = { 0 };
	char scrubbedTargetURI[MAXCONNINFO] = { 0 };

	(void) parse_and_scrub_connection_string(copyDBoptions.source_pguri,
											 scrubbedSourceURI);

	(void) parse_and_scrub_connection_string(copyDBoptions.target_pguri,
											 scrubbedTargetURI);

	log_info("[SOURCE] Copying database from \"%s\"", scrubbedSourceURI);
	log_info("[TARGET] Copying database into \"%s\"", scrubbedTargetURI);

	(void) find_pg_commands(pgPaths);

	log_debug("Using pg_dump for Postgres \"%s\" at \"%s\"",
			  copySpecs->pgPaths.pg_version,
			  copySpecs->pgPaths.pg_dump);

	log_debug("Using pg_restore for Postgres \"%s\" at \"%s\"",
			  copySpecs->pgPaths.pg_version,
			  copySpecs->pgPaths.pg_restore);

	char *dir =
		IS_EMPTY_STRING_BUFFER(copyDBoptions.dir)
		? NULL
		: copyDBoptions.dir;

	bool createWorkDir = true;
	bool service = true;
	char *serviceName = NULL;   /* this is the "main" service */

	/*
	 * Commands that won't set a work directory certainly are not running a
	 * service, they won't even have a pidfile.
	 */
	if (dir == NULL)
	{
		service = false;
	}

	if (!copydb_init_workdir(copySpecs,
							 dir,
							 service,
							 serviceName,
							 copyDBoptions.restart,
							 copyDBoptions.resume,
							 createWorkDir))
	{
		/* errors have already been logged */
		exit(EXIT_CODE_INTERNAL_ERROR);
	}

	if (!copydb_init_specs(copySpecs, &copyDBoptions, section))
	{
		/* errors have already been logged */
		exit(EXIT_CODE_INTERNAL_ERROR);
	}

	if (!IS_EMPTY_STRING_BUFFER(copyDBoptions.filterFileName))
	{
		SourceFilters *filters = &(copySpecs->filters);

		if (!parse_filters(copyDBoptions.filterFileName, filters))
		{
			log_error("Failed to parse filters in file \"%s\"",
					  copyDBoptions.filterFileName);
			exit(EXIT_CODE_BAD_ARGS);
		}
	}
}
