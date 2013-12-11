/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2013 Bert Vermeulen <bert@biot.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "../libsigrokdecode.h"
#include <libsigrok/libsigrok.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <glib.h>
#ifdef __LINUX__
#include <sched.h>
#endif
#include "../config.h"


int debug = FALSE;
int statistics = FALSE;

struct probe {
	char *name;
	int probe;
};

struct option {
	char *key;
	char *value;
};

struct pd {
	char *name;
	GSList *probes;
	GSList *options;
};

struct output {
	char *pd;
	int type;
	char *class;
	int class_idx;
	char *outfile;
	int outfd;
};


void logmsg(char *prefix, FILE *out, const char *format, va_list args)
{
	if (prefix)
		fprintf(out, "%s", prefix);
	vfprintf(out, format, args);
	fprintf(out, "\n");
}

void DBG(const char *format, ...)
{
	va_list args;

	if (!debug)
		return;
	va_start(args, format);
	logmsg("DBG: ", stdout, format, args);
	va_end(args);
}

void ERR(const char *format, ...)
{
	va_list args;

	va_start(args, format);
	logmsg("Error: ", stderr, format, args);
	va_end(args);
}

int srd_log(void *cb_data, int loglevel, const char *format, va_list args)
{
	(void)cb_data;

	if (loglevel == SRD_LOG_ERR || loglevel == SRD_LOG_WARN)
		logmsg("Error: srd: ", stderr, format, args);
	else if (loglevel >= SRD_LOG_DBG && debug)
		logmsg("DBG: srd: ", stdout, format, args);

	return SRD_OK;
}

void usage(char *msg)
{
	if (msg)
		fprintf(stderr, "%s\n", msg);

	//while((c = getopt(argc, argv, "dP:p:o:i:O:f:S")) != -1) {
	printf("Usage: runtc [-dPpoiOf]\n");
	printf("  -d  Debug\n");
	printf("  -P  <protocol decoder>\n");
	printf("  -p  <probename=probenum> (optional)\n");
	printf("  -o  <probeoption=value> (optional)\n");
	printf("  -i <input file>\n");
	printf("  -O <output-pd:output-type[:output-class]>\n");
	printf("  -f <output file> (optional)\n");
	exit(msg ? 1 : 0);

}

static void srd_cb_ann(struct srd_proto_data *pdata, void *cb_data)
{
	struct srd_decoder *dec;
	struct srd_proto_data_annotation *pda;
	struct output *op;
	GString *line;
	int i;
	char **dec_ann;

	DBG("Annotation from %s", pdata->pdo->di->inst_id);
	op = cb_data;
	pda = pdata->data;
	dec = pdata->pdo->di->decoder;

	if (strcmp(pdata->pdo->di->inst_id, op->pd))
		/* This is not the PD selected for output. */
		return;

	if (op->class_idx != -1 && op->class_idx != pda->ann_format)
		/* This output takes a specific annotation class,
		 * but not the one that just came in. */
		return;

	dec_ann = g_slist_nth_data(dec->annotations, pda->ann_format);
	line = g_string_sized_new(256);
	g_string_printf(line, "%"PRIu64"-%"PRIu64" %s: %s:",
			pdata->start_sample, pdata->end_sample,
			pdata->pdo->di->inst_id, dec_ann[0]);
	for (i = 0; pda->ann_text[i]; i++)
		g_string_append_printf(line, " \"%s\"", pda->ann_text[i]);
	g_string_append(line, "\n");
	if (write(op->outfd, line->str, line->len) == -1)
		ERR("Oops!");
	g_string_free(line, TRUE);

}

static void sr_cb(const struct sr_dev_inst *sdi,
		const struct sr_datafeed_packet *packet, void *cb_data)
{
	const struct sr_datafeed_logic *logic;
	struct srd_session *sess;
	GVariant *gvar;
	uint64_t samplerate;
	int num_samples;
	static int samplecnt = 0;

	sess = cb_data;

	switch (packet->type) {
	case SR_DF_HEADER:
		DBG("Received SR_DF_HEADER");
		if (sr_config_get(sdi->driver, sdi, NULL, SR_CONF_SAMPLERATE,
				&gvar) != SR_OK) {
			ERR("Getting samplerate failed");
			break;
		}
		samplerate = g_variant_get_uint64(gvar);
		g_variant_unref(gvar);
		if (srd_session_metadata_set(sess, SRD_CONF_SAMPLERATE,
				g_variant_new_uint64(samplerate)) != SRD_OK) {
			ERR("Setting samplerate failed");
			break;
		}
		if (srd_session_start(sess) != SRD_OK) {
			ERR("Session start failed");
			break;
		}
		break;
	case SR_DF_LOGIC:
		logic = packet->payload;
		num_samples = logic->length / logic->unitsize;
		DBG("Received SR_DF_LOGIC: %d samples", num_samples);
		srd_session_send(sess, samplecnt, samplecnt + num_samples,
				logic->data, logic->length);
		samplecnt += logic->length / logic->unitsize;
		break;
	case SR_DF_END:
		DBG("Received SR_DF_END");
		break;
	}

}

int get_stats(int stats[2])
{
	FILE *f;
	size_t len;
	int tmp;
	char *buf;

	stats[0] = stats[1] = -1;
	if (!(f = fopen("/proc/self/status", "r")))
		return FALSE;
	len = 128;
	buf = malloc(len);
	while (getline(&buf, &len, f) != -1) {
		if (strcasestr(buf, "vmpeak:")) {
			stats[0] = strtoul(buf + 10, NULL, 10);
		} else if (strcasestr(buf, "vmsize:")) {
			tmp = strtoul(buf + 10, NULL, 10);
			if (tmp > stats[0])
				stats[0] = tmp;
		} else if (strcasestr(buf, "vmhwm:")) {
			stats[1] = strtoul(buf + 6, NULL, 10);
		} else if (strcasestr(buf, "vmrss:")) {
			tmp = strtoul(buf + 10, NULL, 10);
			if (tmp > stats[0])
				stats[0] = tmp;
		}
	}
	free(buf);
	fclose(f);

	return TRUE;
}

static int run_testcase(char *infile, GSList *pdlist, struct output *op)
{
	struct srd_session *sess;
	struct srd_decoder *dec;
	struct srd_decoder_inst *di, *prev_di;
	struct pd *pd;
	struct probe *probe;
	struct option *option;
	GVariant *gvar;
	GHashTable *probes, *opts;
	GSList *pdl, *l, *annl;
	int idx;
	char **dec_ann;

	if (op->outfile) {
		if ((op->outfd = open(op->outfile, O_CREAT|O_WRONLY, 0600)) == -1) {
			ERR("Unable to open %s for writing: %s", op->outfile,
					strerror(errno));
			return FALSE;
		}
	}

	if (sr_session_load(infile) != SR_OK)
		return FALSE;

	if (srd_session_new(&sess) != SRD_OK)
		return FALSE;
	sr_session_datafeed_callback_add(sr_cb, sess);
	srd_pd_output_callback_add(sess, SRD_OUTPUT_ANN, srd_cb_ann, op);

	prev_di = NULL;
	pd = NULL;
	for (pdl = pdlist; pdl; pdl = pdl->next) {
		pd = pdl->data;
		if (srd_decoder_load(pd->name) != SRD_OK)
			return FALSE;

		/* Instantiate decoder and pass in options. */
		opts = g_hash_table_new_full(g_str_hash, g_str_equal, NULL,
				(GDestroyNotify)g_variant_unref);
		for (l = pd->options; l; l = l->next) {
			option = l->data;
			g_hash_table_insert(opts, option->key, option->value);
		}
		if (!(di = srd_inst_new(sess, pd->name, opts)))
			return FALSE;
		g_hash_table_destroy(opts);

		/* Map probes. */
		if (pd->probes) {
			probes = g_hash_table_new_full(g_str_hash, g_str_equal, NULL,
					(GDestroyNotify)g_variant_unref);
			for (l = pd->probes; l; l = l->next) {
				probe = l->data;
				gvar = g_variant_new_int32(probe->probe);
				g_variant_ref_sink(gvar);
				g_hash_table_insert(probes, probe->name, gvar);
			}
			if (srd_inst_probe_set_all(di, probes) != SRD_OK)
				return FALSE;
			g_hash_table_destroy(probes);
		}


		/* If this is not the first decoder in the list, stack it
		 * on top of the previous one. */
		if (prev_di) {
			if (srd_inst_stack(sess, prev_di, di) != SRD_OK) {
				ERR("Failed to stack decoder instances.");
				return FALSE;
			}
		}
		prev_di = di;
	}

	/* Resolve top decoder's class index, so we can match. */
	dec = srd_decoder_get_by_id(pd->name);
	if (op->class) {
		if (op->type == SRD_OUTPUT_ANN)
			annl = dec->annotations;
		/* TODO can't dereference this for binary yet
		else if (op->type == SRD_OUTPUT_BINARY)
			annl = dec->binary;
		*/
		else
			/* Only annotations and binary for now. */
			return FALSE;
		idx = 0;
		while(annl) {
			dec_ann = annl->data;
			/* TODO can't dereference this for binary yet */
			if (!strcmp(dec_ann[0], op->class)) {
				op->class_idx = idx;
				break;
			} else
				idx++;
		annl = annl->next;
		}
		if (op->class_idx == -1) {
			ERR("Output class '%s' not found in decoder %s.",
					op->class, pd->name);
			return FALSE;
		}
	}

	sr_session_start();
	sr_session_run();
	sr_session_stop();

	srd_session_destroy(sess);

	if (op->outfile)
		close(op->outfd);

	return TRUE;
}

int main(int argc, char **argv)
{
	struct sr_context *ctx;
	GSList *pdlist;
	struct pd *pd;
	struct probe *probe;
	struct option *option;
	struct output *op;
	char c, *opt_infile, **kv, **opstr;

	op = malloc(sizeof(struct output));
	op->pd = NULL;
	op->type = -1;
	op->class = NULL;
	op->class_idx = -1;
	op->outfd = 1;

	pdlist = NULL;
	opt_infile = NULL;
	pd = NULL;
	while((c = getopt(argc, argv, "dP:p:o:i:O:f:S")) != -1) {
		switch(c) {
		case 'd':
			debug = TRUE;
			break;
		case 'P':
			pd = g_malloc(sizeof(struct pd));
			pd->name = g_strdup(optarg);
			pd->probes = pd->options = NULL;
			pdlist = g_slist_append(pdlist, pd);
			break;
		case 'p':
		case 'o':
			if (g_slist_length(pdlist) == 0) {
				/* No previous -P. */
				ERR("Syntax error at '%s'", optarg);
				usage(NULL);
			}
			kv = g_strsplit(optarg, "=", 0);
			if (!kv[0] || (!kv[1] || kv[2])) {
				/* Need x=y. */
				ERR("Syntax error at '%s'", optarg);
				g_strfreev(kv);
				usage(NULL);
			}
			if (c == 'p') {
				probe = malloc(sizeof(struct probe));
				probe->name = g_strdup(kv[0]);
				probe->probe = strtoul(kv[1], 0, 10);
				/* Apply to last PD. */
				pd->probes = g_slist_append(pd->probes, probe);
			} else {
				option = malloc(sizeof(struct option));
				option->key = g_strdup(kv[0]);
				option->value = g_strdup(kv[1]);
				/* Apply to last PD. */
				pd->options = g_slist_append(pd->options, option);
			}
			break;
		case 'i':
			opt_infile = optarg;
			break;
		case 'O':
			opstr = g_strsplit(optarg, ":", 0);
			if (!opstr[0] || !opstr[1]) {
				/* Need at least abc:def. */
				ERR("Syntax error at '%s'", optarg);
				g_strfreev(opstr);
				usage(NULL);
			}
			op->pd = g_strdup(opstr[0]);
			if (!strcmp(opstr[1], "annotation"))
				op->type = SRD_OUTPUT_ANN;
			else if (!strcmp(opstr[1], "binary"))
				op->type = SRD_OUTPUT_BINARY;
			else if (!strcmp(opstr[1], "python"))
				op->type = SRD_OUTPUT_PYTHON;
			else {
				ERR("Unknown output type '%s'", opstr[1]);
				g_strfreev(opstr);
				usage(NULL);
			}
			if (opstr[2])
				op->class = g_strdup(opstr[2]);
			g_strfreev(opstr);
			break;
		case 'f':
			op->outfile = g_strdup(optarg);
			op->outfd = -1;
			break;
		case 'S':
			statistics = TRUE;
			break;
		default:
			usage(NULL);
		}
	}
	if (argc > optind)
		usage(NULL);
	if (g_slist_length(pdlist) == 0)
		usage(NULL);
	if (!opt_infile)
		usage(NULL);
	if (!op->pd || op->type == -1)
		usage(NULL);

	if (sr_init(&ctx) != SR_OK)
		return 1;

	srd_log_callback_set(srd_log, NULL);
	if (srd_init(DECODERS_DIR) != SRD_OK)
		return 1;

	run_testcase(opt_infile, pdlist, op);

	srd_exit();
	sr_exit(ctx);

	return 0;
}

