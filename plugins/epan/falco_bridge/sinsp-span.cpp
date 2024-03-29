/* sinsp-span.cpp
 *
 * By Gerald Combs
 * Copyright (C) 2022 Sysdig, Inc.
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <stddef.h>
#include <stdint.h>

#include <glib.h>

#include <epan/wmem_scopes.h>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable:4100)
#pragma warning(disable:4267)
#endif

// To do:
// [.] Shrink sinsp_field_extract_t:
//     [.] Create separate syscall_extract_t and plugin_extract_t structs? We probably don't need the
//         field name, type, or is_present.
//     [ ] Dup short bytes in the struct and intern longer ones somewhere?
// [ ] Don't cache some fields, e.g. event time


// epan/address.h and driver/ppm_events_public.h both define PT_NONE, so
// handle libsinsp calls here.

typedef struct hf_register_info hf_register_info;

typedef struct ss_plugin_info ss_plugin_info;

#include "sinsp-span.h"

#include <sinsp.h>

typedef struct sinsp_source_info_t {
    sinsp_plugin *source;
    std::vector<const filter_check_info *> syscall_filter_checks;
    std::vector<gen_event_filter_check *> syscall_event_filter_checks;
    std::vector<const filtercheck_field_info *> syscall_filter_fields;
    std::map<const filtercheck_field_info *, size_t> ffi_to_sf_idx;
    std::map<size_t, sinsp_syscall_category_e> field_to_category;
    sinsp_evt *evt;
    uint8_t *evt_storage;
    size_t evt_storage_size;
    const char *name;
    const char *description;
    char *last_error;
} sinsp_source_info_t;

static const size_t sfe_slab_prealloc = 250000;

typedef struct sinsp_span_t {
    sinsp inspector;
    sinsp_filter_check_list filter_checks;
    sinsp_field_extract_t *sfe_slab;
    size_t sfe_slab_offset;
    // XXX Combine these into a single struct?
    std::vector<sinsp_field_extract_t *> sfe_ptrs;
    std::vector<uint16_t> sfe_lengths;
    std::vector<const ppm_event_info*> sfe_infos;
    // Interned data. Copied from maxmind_db.c.
    wmem_map_t *str_chunk;
} sinsp_span_t;

#define SS_MEMORY_STATISTICS 1

#ifdef SS_MEMORY_STATISTICS
#include <wsutil/str_util.h>

static int alloc_sfe_bytes;
static int unused_sfe_bytes;
static int total_chunked_strings;
static int total_bytes;
#endif

static filter_check_info g_args_fci;

sinsp_span_t *create_sinsp_span()
{
    sinsp_span_t *span = new(sinsp_span_t);
    span->inspector.set_internal_events_mode(true);

    return span;
}

void destroy_sinsp_span(sinsp_span_t *sinsp_span) {
    delete(sinsp_span);
}

static const char *chunkify_string(sinsp_span_t *sinsp_span, const char *key) {
    char *chunk_string = (char *) wmem_map_lookup(sinsp_span->str_chunk, key);

    if (!chunk_string) {
        chunk_string = wmem_strdup(wmem_file_scope(), key);
        wmem_map_insert(sinsp_span->str_chunk, chunk_string, chunk_string);
    }

    return chunk_string;
}

static sinsp_syscall_category_e filtercheck_name_to_category(const std::string fc_name) {
    std::map<const char *, sinsp_syscall_category_e> fc_name_to_category = {
        { "evt", SSC_EVENT },
        { "args", SSC_ARGS },
        { "process", SSC_PROCESS },
        { "user", SSC_USER },
        { "group", SSC_GROUP },
        { "container", SSC_CONTAINER },
        { "fd", SSC_FD },
        { "fs.path", SSC_FS },
        // syslog collides with the dissector
        { "fdlist", SSC_FDLIST },
    };

    for (const auto ptc : fc_name_to_category) {
        if (ptc.first == fc_name) {
            return ptc.second;
        }
    }
    return SSC_OTHER;
}

const filtercheck_field_info args_event_fields[] =
{
	{PT_CHARBUF, EPF_NONE, PF_NA, "evt.arg.0", "Argument", "Event argument."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "evt.arg.1", "Argument", "Event argument."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "evt.arg.2", "Argument", "Event argument."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "evt.arg.3", "Argument", "Event argument."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "evt.arg.4", "Argument", "Event argument."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "evt.arg.5", "Argument", "Event argument."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "evt.arg.6", "Argument", "Event argument."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "evt.arg.7", "Argument", "Event argument."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "evt.arg.8", "Argument", "Event argument."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "evt.arg.9", "Argument", "Event argument."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "evt.arg.10", "Argument", "Event argument."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "evt.arg.11", "Argument", "Event argument."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "evt.arg.12", "Argument", "Event argument."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "evt.arg.13", "Argument", "Event argument."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "evt.arg.14", "Argument", "Event argument."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "evt.arg.15", "Argument", "Event argument."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "evt.arg.16", "Argument", "Event argument."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "evt.arg.17", "Argument", "Event argument."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "evt.arg.18", "Argument", "Event argument."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "evt.arg.19", "Argument", "Event argument."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "evt.arg.20", "Argument", "Event argument."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "evt.arg.21", "Argument", "Event argument."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "evt.arg.22", "Argument", "Event argument."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "evt.arg.23", "Argument", "Event argument."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "evt.arg.24", "Argument", "Event argument."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "evt.arg.25", "Argument", "Event argument."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "evt.arg.26", "Argument", "Event argument."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "evt.arg.27", "Argument", "Event argument."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "evt.arg.28", "Argument", "Event argument."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "evt.arg.29", "Argument", "Event argument."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "evt.arg.30", "Argument", "Event argument."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "evt.arg.31", "Argument", "Event argument."},
};

void add_arg_event(uint32_t arg_number,
        sinsp_filter_factory* filter_factory,
        sinsp_source_info_t *ssi,
        sinsp_syscall_category_e args_syscall_category) {

    if (arg_number >= sizeof(args_event_fields) / sizeof(args_event_fields[0])) {
        ws_error("falco event has too many arguments (%" PRIu32 ")", arg_number);
    }

    std::string fname = "evt.arg[" + std::to_string(arg_number) + "]";

    const filtercheck_field_info *ffi = &args_event_fields[arg_number];
    gen_event_filter_check *gefc = filter_factory->new_filtercheck(fname.c_str());
    if (!gefc) {
        ws_error("cannot find expected Falco field evt.arg");
    }
    gefc->parse_field_name(fname.c_str(), true, false);
    ssi->ffi_to_sf_idx[ffi] = ssi->syscall_filter_fields.size();
    ssi->field_to_category[ssi->syscall_filter_fields.size()] = args_syscall_category;
    ssi->syscall_event_filter_checks.push_back(gefc);
    ssi->syscall_filter_fields.push_back(ffi);
}

/*
 * Populate a sinsp_source_info_t struct with the symbols coming from libsinsp's builtin syscall extractors
 */
void create_sinsp_syscall_source(sinsp_span_t *sinsp_span, sinsp_source_info_t **ssi_ptr) {
    sinsp_source_info_t *ssi = new sinsp_source_info_t();

    std::shared_ptr<gen_event_filter_factory> factory(new sinsp_filter_factory(NULL, sinsp_span->filter_checks));
    sinsp_filter_factory filter_factory(&sinsp_span->inspector, sinsp_span->filter_checks);
    std::vector<const filter_check_info*> all_syscall_fields;

    // Extract the fields defined in filterchecks.{cpp,h}
    sinsp_span->filter_checks.get_all_fields(all_syscall_fields);
    for (const auto fci : all_syscall_fields) {
        if (fci->m_flags == filter_check_info::FL_HIDDEN) {
            continue;
        }

        if (fci->m_name == "process") {
            // This creates a meta-filtercheck for the events arguments and it register its fields.
            // We do it before the process filtercheck because we want to have it exactly in the position
            // after event and before process.
            g_args_fci.m_name = "args";
            sinsp_syscall_category_e args_syscall_category = filtercheck_name_to_category(g_args_fci.m_name);

            g_args_fci = *fci;

            for (uint32_t i = 0; i < 32; i++) {
                add_arg_event(i, &filter_factory, ssi, args_syscall_category);
            }
            ssi->syscall_filter_checks.push_back(&g_args_fci);
        }

        sinsp_syscall_category_e syscall_category = filtercheck_name_to_category(fci->m_name);

        for (int i = 0; i < fci->m_nfields; i++) {
            const filtercheck_field_info *ffi = &fci->m_fields[i];
            if (ffi->m_flags == filtercheck_field_flags::EPF_NONE) {
                gen_event_filter_check *gefc = filter_factory.new_filtercheck(ffi->m_name);
                if (!gefc) {
                    continue;
                }
                gefc->parse_field_name(ffi->m_name, true, false);
                ssi->ffi_to_sf_idx[ffi] = ssi->syscall_filter_fields.size();
                ssi->field_to_category[ssi->syscall_filter_fields.size()] = syscall_category;
                ssi->syscall_event_filter_checks.push_back(gefc);
                ssi->syscall_filter_fields.push_back(ffi);
            }
        }

        ssi->syscall_filter_checks.push_back(fci);
    }

    ssi->evt = new sinsp_evt(&sinsp_span->inspector);
    ssi->evt_storage_size = 4096;
    ssi->evt_storage = (uint8_t *) g_malloc(ssi->evt_storage_size);
    ssi->name = strdup(sinsp_syscall_event_source_name);
    ssi->description = strdup(sinsp_syscall_event_source_name);
    *ssi_ptr = ssi;
    return;
}

/*
 * Populate a sinsp_source_info_t struct with the symbols coming from a library loaded via libsinsp
 */
char *
create_sinsp_plugin_source(sinsp_span_t *sinsp_span, const char* libname, sinsp_source_info_t **ssi_ptr)
{
    sinsp_source_info_t *ssi = new sinsp_source_info_t();

    char *err_str = NULL;
    try {
        auto sp = sinsp_span->inspector.register_plugin(libname);
        if (sp->caps() & CAP_EXTRACTION) {
            ssi->source = dynamic_cast<sinsp_plugin *>(sp.get());
        } else {
            err_str = g_strdup_printf("%s has unsupported plugin capabilities 0x%02x", libname, sp->caps());
        }
    } catch (const sinsp_exception& e) {
        err_str = g_strdup_printf("Caught sinsp exception %s", e.what());
    }

    std::string init_err;
    if (!err_str) {
        if (!ssi->source->init("{}", init_err)) {
            err_str = g_strdup_printf("Unable to initialize %s: %s", libname, init_err.c_str());
        }
    }
    if (err_str) {
        delete ssi;
        return err_str;
    }

    ssi->evt = new sinsp_evt(&sinsp_span->inspector);
    ssi->evt_storage_size = 4096;
    ssi->evt_storage = (uint8_t *) g_malloc(ssi->evt_storage_size);
    ssi->name = strdup(ssi->source->name().c_str());
    ssi->description = strdup(ssi->source->description().c_str());
    *ssi_ptr = ssi;
    return NULL;
}

uint32_t get_sinsp_source_id(sinsp_source_info_t *ssi)
{
    if (ssi->source) {
        return ssi->source->id();
    }
    return 0;
}

const char *get_sinsp_source_last_error(sinsp_source_info_t *ssi)
{
    if (ssi->source) {
        if (ssi->last_error) {
            free(ssi->last_error);
        }
        ssi->last_error = strdup(ssi->source->get_last_error().c_str());
    }
    return ssi->last_error;
}

const char *get_sinsp_source_name(sinsp_source_info_t *ssi)
{
    return ssi->name;
}

const char *get_sinsp_source_description(sinsp_source_info_t *ssi)
{
    return ssi->description;
}

size_t get_sinsp_source_nfields(sinsp_source_info_t *ssi)
{
    if (ssi->source) {
        return ssi->source->fields().size();
    }

    return ssi->syscall_filter_fields.size();
}

bool get_sinsp_source_field_info(sinsp_source_info_t *ssi, size_t field_num, sinsp_field_info_t *field)
{
    if (field_num >= get_sinsp_source_nfields(ssi)) {
        return false;
    }

    const filtercheck_field_info *ffi = NULL;

    if (ssi->source) {
        ffi = &ssi->source->fields()[field_num];
        g_strlcpy(field->abbrev, ffi->m_name, sizeof(field->abbrev));
    } else {
        ffi = ssi->syscall_filter_fields[field_num];
        if (ssi->field_to_category[field_num] == SSC_OTHER) {
            snprintf(field->abbrev, sizeof(field->abbrev), FALCO_FIELD_NAME_PREFIX "%s", ffi->m_name);
        } else {
            g_strlcpy(field->abbrev, ffi->m_name, sizeof(field->abbrev));
        }
    }

    g_strlcpy(field->display, ffi->m_display, sizeof(field->display));
    g_strlcpy(field->description, ffi->m_description, sizeof(field->description));

    field->is_hidden = ffi->m_flags & EPF_TABLE_ONLY;
    field->is_conversation = ffi->m_flags & EPF_CONVERSATION;
    field->is_info = ffi->m_flags & EPF_INFO;

    field->is_numeric_address = false;

    switch (ffi->m_type) {
    case PT_INT8:
        field->type = FT_INT8;
        break;
    case PT_INT16:
        field->type = FT_INT16;
        break;
    case PT_INT32:
        field->type = FT_INT32;
        break;
    case PT_INT64:
        field->type = FT_INT64;
        break;
    case PT_UINT8:
        field->type = FT_UINT8;
        break;
    case PT_UINT16:
    case PT_PORT:
        field->type = FT_UINT16;
        break;
    case PT_UINT32:
        field->type = FT_UINT32;
        break;
    case PT_UINT64:
    case PT_RELTIME:
    case PT_ABSTIME:
        field->type = FT_UINT64;
        break;
    case PT_CHARBUF:
        field->type = FT_STRINGZ;
        break;
//        field->type = FT_RELATIVE_TIME;
//        break;
//        field->type = FT_ABSOLUTE_TIME;
//        field->type = FT_UINT64;
//        field->display_format = SFDF_DECIMAL;
        break;
    case PT_BYTEBUF:
        field->type = FT_BYTES;
    case PT_BOOL:
        field->type = FT_BOOLEAN;
        break;
    case PT_DOUBLE:
        field->type = FT_DOUBLE;
        break;
    case PT_IPADDR:
        field->type = FT_BYTES;
        field->is_numeric_address = true;
        break;
    default:
        ws_debug("Unknown Falco parameter type %d for %s", ffi->m_type, field->abbrev);
        field->type = FT_BYTES;
    }

    switch (ffi->m_print_format) {
    case PF_DEC:
    case PF_10_PADDED_DEC:
    case PF_ID:
        field->display_format = SFDF_DECIMAL;
        break;
    case PF_HEX:
        field->display_format = SFDF_HEXADECIMAL;
        break;
    case PF_OCT:
        field->display_format = SFDF_OCTAL;
        break;
    default:
        field->display_format = SFDF_UNKNOWN;
        break;
    }

    return true;
}

char* get_evt_arg_name(void* sinp_evt_info, uint32_t arg_num) {
    ppm_event_info* realinfo = (ppm_event_info*)sinp_evt_info;

    if (arg_num > realinfo->nparams) {
        ws_error("Arg number %u exceeds event parameter count %u", arg_num, realinfo->nparams);
        return NULL;
    }
    return realinfo->params[arg_num].name;
}

void open_sinsp_capture(sinsp_span_t *sinsp_span, const char *filepath)
{
    sinsp_span->sfe_slab = NULL;
    sinsp_span->sfe_slab_offset = 0;
    sinsp_span->sfe_ptrs.clear();
    sinsp_span->sfe_lengths.clear();
    sinsp_span->sfe_infos.clear();
    sinsp_span->inspector.open_savefile(filepath);
    sinsp_span->str_chunk = wmem_map_new(wmem_file_scope(), wmem_str_hash, g_str_equal);

#ifdef SS_MEMORY_STATISTICS
    alloc_sfe_bytes = 0;
    unused_sfe_bytes = 0;
    total_chunked_strings = 0;
    total_bytes = 0;
#endif
}

static void add_syscall_event_to_cache(sinsp_span_t *sinsp_span, sinsp_source_info_t *ssi, sinsp_evt *evt)
{
    uint64_t evt_num = evt->get_num();

    // Fill in any gaps
    if (evt_num > 1 && evt_num - 1 > sinsp_span->sfe_ptrs.size()) {
        ws_debug("Filling syscall gap from %d to %u", (int) sinsp_span->sfe_ptrs.size(), (unsigned) evt_num - 1);
        sinsp_span->sfe_ptrs.resize(evt_num - 1);
        sinsp_span->sfe_lengths.resize(evt_num - 1);
        sinsp_span->sfe_infos.resize(evt_num - 1);
    }

    // libsinsp requires that events be processed in order so we cache our extracted
    // data during the first pass. We don't know how many fields we're going to extract
    // during an event, so we preallocate slabs of `sfe_slab_prealloc` entries.
    //
    // XXX This assumes that we won't extract more than ssi->syscall_event_filter_checks.size()
    // fields per event.
    if (sinsp_span->sfe_slab_offset + ssi->syscall_event_filter_checks.size() > sfe_slab_prealloc) {
#ifdef SS_MEMORY_STATISTICS
        if (sinsp_span->sfe_slab_offset > 0) {
            unused_sfe_bytes += sizeof(sinsp_field_extract_t) * (sfe_slab_prealloc - sinsp_span->sfe_slab_offset);
        }
#endif
        sinsp_span->sfe_slab = NULL;
        sinsp_span->sfe_slab_offset = 0;
    }

    if (sinsp_span->sfe_slab == NULL) {
#ifdef SS_MEMORY_STATISTICS
        alloc_sfe_bytes += sizeof(sinsp_field_extract_t) * sfe_slab_prealloc;
#endif
        sinsp_span->sfe_slab = (sinsp_field_extract_t *) wmem_alloc(wmem_file_scope(), sizeof(sinsp_field_extract_t) * sfe_slab_prealloc);
    }

    sinsp_field_extract_t *sfe_block = &sinsp_span->sfe_slab[sinsp_span->sfe_slab_offset];
    std::vector<extract_value_t> values;
    uint16_t sfe_idx = 0;

    for (size_t fc_idx = 0; fc_idx < ssi->syscall_event_filter_checks.size(); fc_idx++) {
        auto gefc = ssi->syscall_event_filter_checks[fc_idx];
        values.clear();
        if (!gefc->extract(evt, values, false) || values.size() < 1) {
            continue;
        }
        auto ffi = ssi->syscall_filter_fields[fc_idx];
        if (ffi->m_flags == filtercheck_field_flags::EPF_NONE && values[0].len > 0) {
            if (sinsp_span->sfe_slab_offset + sfe_idx >= sfe_slab_prealloc) {
                ws_error("Extracting too many fields for event %u (%d vs %d)", (unsigned) evt->get_num(), (int) sfe_idx, (int) ssi->syscall_event_filter_checks.size());
            }

            sinsp_field_extract_t *sfe = &sfe_block[sfe_idx];
            sfe_idx++;
            sfe->field_idx = (uint32_t) fc_idx;
            // XXX Use memcpy instead of all this casting?
            switch (ffi->m_type) {
            case PT_INT8:
                sfe->res.i32 = *(int8_t*)values[0].ptr;
                break;
            case PT_INT16:
                sfe->res.i32 = *(int16_t*)values[0].ptr;
                break;
            case PT_INT32:
                sfe->res.i32 = *(int32_t*)values[0].ptr;
                break;
            case PT_INT64:
                sfe->res.i64 = *(int64_t *)values[0].ptr;
                break;
            case PT_UINT8:
                sfe->res.u32 = *(uint8_t*)values[0].ptr;
                break;
            case PT_UINT16:
            case PT_PORT:
                sfe->res.u32 = *(int16_t*)values[0].ptr;
                break;
            case PT_UINT32:
                sfe->res.u32 = *(int32_t*)values[0].ptr;
                break;
            case PT_UINT64:
            case PT_RELTIME:
            case PT_ABSTIME:
                sfe->res.u64 = *(uint64_t *)values[0].ptr;
                break;
            case PT_CHARBUF:
                if (values[0].len < SFE_SMALL_BUF_SIZE) {
                    g_strlcpy(sfe->res.small_str, (const char *) values[0].ptr, SFE_SMALL_BUF_SIZE);
                } else {
                    sfe->res.str = chunkify_string(sinsp_span, (const char *) values[0].ptr);
                    total_chunked_strings += values[0].len;
                }
                // XXX - Not needed? This sometimes runs into length mismatches.
                // sfe_value.res.str[values[0].len] = '\0';
                break;
            case PT_BOOL:
                sfe->res.boolean = (bool)(uint32_t) *(uint32_t*)values[0].ptr;
                break;
            case PT_DOUBLE:
                sfe->res.dbl = *(double*)values[0].ptr;
                break;
            default:
                sfe->res.bytes = (uint8_t*) wmem_memdup(wmem_file_scope(), (const uint8_t *) values[0].ptr, values[0].len);
                total_bytes += values[0].len;
            }

            sfe->res_len = values[0].len;
        }
    }

    sinsp_span->sfe_slab_offset += sfe_idx;
    sinsp_span->sfe_ptrs.push_back(sfe_block);
    sinsp_span->sfe_lengths.push_back(sfe_idx);
    sinsp_span->sfe_infos.push_back(evt->get_info());

    if (sinsp_span->sfe_ptrs.size() < evt_num) {
        ws_warning("Unable to fill cache to the proper size (%d vs %u)", (int) sinsp_span->sfe_ptrs.size(), (unsigned) evt_num);
    }

    return;
}

void close_sinsp_capture(sinsp_span_t *sinsp_span)
{
#ifdef SS_MEMORY_STATISTICS
    unused_sfe_bytes += sizeof(sinsp_field_extract_t) * sfe_slab_prealloc - sinsp_span->sfe_slab_offset;

    g_warning("Allocated sinsp_field_extract_t bytes: %s", format_size(alloc_sfe_bytes, FORMAT_SIZE_UNIT_BYTES, FORMAT_SIZE_PREFIX_SI));
    g_warning("Unused sinsp_field_extract_t bytes: %s", format_size(unused_sfe_bytes, FORMAT_SIZE_UNIT_BYTES, FORMAT_SIZE_PREFIX_SI));
    g_warning("Chunked string bytes: %s", format_size(total_chunked_strings, FORMAT_SIZE_UNIT_BYTES, FORMAT_SIZE_PREFIX_SI));
    g_warning("Byte value (I/O) bytes: %s", format_size(total_bytes, FORMAT_SIZE_UNIT_BYTES, FORMAT_SIZE_PREFIX_SI));
    g_warning("Cache capacity: %s items, sinsp_field_extract_t pointer bytes = %s, length bytes = %s",
              format_size(sinsp_span->sfe_ptrs.capacity(), FORMAT_SIZE_UNIT_BYTES, FORMAT_SIZE_PREFIX_SI),
              format_size(sinsp_span->sfe_ptrs.capacity() * sizeof(sinsp_field_extract_t *), FORMAT_SIZE_UNIT_BYTES, FORMAT_SIZE_PREFIX_SI),
              format_size(sinsp_span->sfe_ptrs.capacity() * sizeof(uint16_t), FORMAT_SIZE_UNIT_BYTES, FORMAT_SIZE_PREFIX_SI));

    alloc_sfe_bytes = 0;
    unused_sfe_bytes = 0;
    total_chunked_strings = 0;
    total_bytes = 0;
#endif

    sinsp_span->inspector.close();
    sinsp_span->sfe_ptrs.clear();
    sinsp_span->sfe_lengths.clear();
    sinsp_span->sfe_infos.clear();
    sinsp_span->str_chunk = NULL;
}

sinsp_syscall_category_e get_syscall_parent_category(sinsp_source_info_t *ssi, size_t field_check_idx)
{
    if (field_check_idx < ssi->field_to_category.size()) {
        return ssi->field_to_category[field_check_idx];
    }
    return SSC_OTHER;
}

// Either have separate cached / non-cached params or pass a pointer to a pointer array.
bool extract_syscall_source_fields(sinsp_span_t *sinsp_span, sinsp_source_info_t *ssi, uint32_t frame_num, sinsp_field_extract_t **sinsp_fields, uint32_t *sinsp_field_len, void** sinp_evt_info) {
    if (ssi->source) {
        return false;
    }

    // libsinsp event numbers may or may not be contiguous. Make sure our event cache is at
    // least as large as the current frame number. add_syscall_event_to_cache will fill in
    // any gaps with null entries.
    while (frame_num > sinsp_span->sfe_ptrs.size()) {
        sinsp_evt *evt = NULL;
        try {
            int32_t res = sinsp_span->inspector.next(&evt);
            switch (res) {
            case SCAP_TIMEOUT:
            case SCAP_FILTERED_EVENT:
                break;
            case SCAP_EOF:
                ws_warning("Unexpected EOF");
                return false;
            case SCAP_SUCCESS:
                add_syscall_event_to_cache(sinsp_span, ssi, evt);
                break;
            default:
                ws_warning("%s", sinsp_span->inspector.getlasterr().c_str());
                return false;
            }
        } catch (sinsp_exception &e) {
            ws_warning("%s", e.what());
            return false;
        }
    }

    // Shouldn't happen
    if (frame_num > sinsp_span->sfe_ptrs.size()) {
        ws_error("Frame number %u exceeds cache size %d", frame_num, (int) sinsp_span->sfe_ptrs.size());
        return false;
    }

    *sinsp_fields = sinsp_span->sfe_ptrs[frame_num - 1];
    *sinsp_field_len = sinsp_span->sfe_lengths[frame_num - 1];
    *sinp_evt_info = (void*)sinsp_span->sfe_infos[frame_num - 1];

    return true;
}

// The code below, falcosecurity/libs, and falcosecurity/plugins need to be in alignment.
// The Makefile in /plugins defines FALCOSECURITY_LIBS_REVISION and uses that version of
// plugin_info.h. We need to build against a compatible revision of /libs.
bool extract_plugin_source_fields(sinsp_source_info_t *ssi, uint32_t event_num, uint8_t *evt_data, uint32_t evt_datalen, wmem_allocator_t *pool, plugin_field_extract_t *sinsp_fields, uint32_t sinsp_field_len)
{
    if (!ssi->source) {
        return false;
    }

    std::vector<ss_plugin_extract_field> fields;

    // PPME_PLUGINEVENT_E events have the following format:
    // | scap_evt header | uint32_t sizeof(id) = 4 | uint32_t evt_datalen | uint32_t id | uint8_t[] evt_data |

    uint32_t payload_hdr[3] = {4, evt_datalen, ssi->source->id()};
//    uint32_t payload_hdr_size = (nparams + 1) * 4;
    uint32_t tot_evt_len = (uint32_t)sizeof(scap_evt) + sizeof(payload_hdr) + evt_datalen;
    if (ssi->evt_storage_size < tot_evt_len) {
        while (ssi->evt_storage_size < tot_evt_len) {
            ssi->evt_storage_size *= 2;
        }
        ssi->evt_storage = (uint8_t *) g_realloc(ssi->evt_storage, ssi->evt_storage_size);
    }
    scap_evt *sevt = (scap_evt *) ssi->evt_storage;

    sevt->ts = -1;
    sevt->tid = -1;
    sevt->len = tot_evt_len;
    sevt->type = PPME_PLUGINEVENT_E;
    sevt->nparams = 2; // Plugin ID + evt_data;

    memcpy(ssi->evt_storage + sizeof(scap_evt), payload_hdr, sizeof(payload_hdr));
    memcpy(ssi->evt_storage + sizeof(scap_evt) + sizeof(payload_hdr), evt_data, evt_datalen);
    ssi->evt->init(ssi->evt_storage, 0);
    ssi->evt->set_num(event_num);

    fields.resize(sinsp_field_len);
    // We must supply field_id, field, arg, and type.
    for (size_t i = 0; i < sinsp_field_len; i++) {
        fields.at(i).field_id = sinsp_fields[i].field_id;
        fields.at(i).field = sinsp_fields[i].field_name;
        if (sinsp_fields[i].type == FT_STRINGZ) {
            fields.at(i).ftype = FTYPE_STRING;
        } else {
            fields.at(i).ftype = FTYPE_UINT64;
        }
    }

    bool status = true;
    if (!ssi->source->extract_fields(ssi->evt, sinsp_field_len, fields.data())) {
        status = false;
    }

    for (size_t i = 0; i < sinsp_field_len; i++) {
        sinsp_fields[i].is_present = fields.at(i).res_len > 0;
        if (sinsp_fields[i].is_present) {
            if (fields.at(i).ftype == PT_CHARBUF) {
                sinsp_fields[i].res.str = wmem_strdup(pool, *fields.at(i).res.str);
            } else if (fields.at(i).ftype == PT_UINT64) {
                sinsp_fields[i].res.u64 = *fields.at(i).res.u64;
            } else {
                status = false;
            }
        }
    }
    return status;
}

#ifdef _MSC_VER
#pragma warning(pop)
#endif
