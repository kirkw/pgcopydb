/*
 * src/bin/pgcopydb/cli_restore.c
 *     Implementation of a CLI which lets you run individual routines
 *     directly
 */

#include <errno.h>
#include <getopt.h>
#include <inttypes.h>

#include "cli_common.h"
#include "cli_restore.h"
#include "cli_root.h"
#include "commandline.h"
#include "log.h"
#include "pgcmd.h"
#include "pgsql.h"
#include "string_utils.h"

RestoreDBOptions restoreDBoptions = { 0 };

static int cli_restore_db_getopts(int argc, char **argv);
static void cli_restore_db(int argc, char **argv);

static CommandLine restore_db_command =
	make_command(
		"db",
		"Restore an entire database from source to target",
		" --source ... --target ... ",
		"  --source          Directory where to save the restore files\n"
		"  --target          Postgres URI to the source database\n",
		cli_restore_db_getopts,
		cli_restore_db);

static CommandLine restore_table_command =
	make_command(
		"table",
		"Restore a given table from source to target",
		" --source ... --target ... ",
		"  --source          Directory where to save the restore files\n"
		"  --target          Postgres URI to the source database\n"
		"  --schema-name     Name of the schema where to find the table\n"
		"  --table-name      Name of the target table\n",
		cli_restore_db_getopts,
		cli_restore_db);


static CommandLine *restore_subcommands[] = {
	&restore_db_command,
	&restore_table_command,
	NULL
};

CommandLine restore_commands =
	make_command_set("restore",
					 "Restore database objects into a Postgres instance",
					 NULL, NULL, NULL, restore_subcommands);


/*
 * cli_restore_db_getopts parses the CLI options for the `restore db` command.
 */
static int
cli_restore_db_getopts(int argc, char **argv)
{
	RestoreDBOptions options = { 0 };
	int c, option_index = 0;
	int errors = 0, verboseCount = 0;

	static struct option long_options[] = {
		{ "source", required_argument, NULL, 'S' },
		{ "target", required_argument, NULL, 'T' },
		{ "schema", required_argument, NULL, 's' },
		{ "table", required_argument, NULL, 't' },
		{ "version", no_argument, NULL, 'V' },
		{ "verbose", no_argument, NULL, 'v' },
		{ "quiet", no_argument, NULL, 'q' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 }
	};

	optind = 0;

	/* install default values */
	strlcpy(options.schema_name, "public", NAMEDATALEN);

	while ((c = getopt_long(argc, argv, "S:T:j:s:t:Vvqh",
							long_options, &option_index)) != -1)
	{
		switch (c)
		{
			case 'S':
			{
				strlcpy(options.source_dir, optarg, MAXPGPATH);
				log_trace("--source %s", options.source_dir);
				break;
			}

			case 'T':
			{
				if (!validate_connection_string(optarg))
				{
					log_fatal("Failed to parse --target connection string, "
							  "see above for details.");
					exit(EXIT_CODE_BAD_ARGS);
				}
				strlcpy(options.target_pguri, optarg, MAXCONNINFO);
				log_trace("--target %s", options.target_pguri);
				break;
			}

			case 's':
			{
				strlcpy(options.schema_name, optarg, NAMEDATALEN);
				log_trace("--schema %s", options.schema_name);
				break;
			}

			case 't':
			{
				strlcpy(options.table_name, optarg, NAMEDATALEN);
				log_trace("--table %s", options.table_name);
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
						log_set_level(LOG_INFO);
						break;
					}

					case 2:
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
		}
	}

	if (IS_EMPTY_STRING_BUFFER(options.source_dir))
	{
		log_fatal("Option --source is mandatory");
		++errors;
	}

	if (IS_EMPTY_STRING_BUFFER(options.target_pguri))
	{
		log_fatal("Option --target is mandatory");
		++errors;
	}

	if (errors > 0)
	{
		exit(EXIT_CODE_BAD_ARGS);
	}

	/* publish our option parsing in the global variable */
	restoreDBoptions = options;

	return optind;
}


/*
 * cli_restore_db implements the command: pgrestoredb restore db
 */
static void
cli_restore_db(int argc, char **argv)
{
	PostgresPaths pgPaths = { 0 };

	log_info("Restoring database from \"%s\"", restoreDBoptions.source_dir);
	log_info("Restoring database into directory \"%s\"",
			 restoreDBoptions.target_pguri);

	(void) find_pg_commands(&pgPaths);

	log_info("Using pg_restore for Postgres \"%s\" at \"%s\"",
			 pgPaths.pg_version,
			 pgPaths.pg_restore);

	char *dir = restoreDBoptions.source_dir;
	char preFilename[MAXPGPATH] = { 0 };

	sformat(preFilename, MAXPGPATH, "%s/%s", dir, "pre.dump");

	if (!file_exists(preFilename))
	{
		log_fatal("File \"%s\" does not exists", preFilename);
		exit(EXIT_CODE_BAD_ARGS);
	}

	if (!pg_restore_db(&pgPaths,
					   restoreDBoptions.target_pguri,
					   preFilename))
	{
		/* errors have already been logged */
		exit(EXIT_CODE_SOURCE);
	}
}
