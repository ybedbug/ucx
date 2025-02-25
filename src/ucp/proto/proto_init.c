/**
 * Copyright (C) Mellanox Technologies Ltd. 2021.  ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "proto_init.h"
#include "proto_debug.h"
#include "proto_select.inl"

#include <ucs/datastruct/array.inl>
#include <ucs/sys/math.h>
#include <ucs/sys/string.h>
#include <ucs/debug/log.h>
#include <float.h>


/* Compare two protocols which intersect at point X, by examining their value
 * at point (X + UCP_PROTO_MSGLEN_EPSILON)
 */
#define UCP_PROTO_MSGLEN_EPSILON   0.5

UCS_ARRAY_IMPL(ucp_proto_perf_envelope, unsigned,
               ucp_proto_perf_envelope_elem_t, static);
UCS_ARRAY_IMPL(ucp_proto_perf_list, unsigned, ucs_linear_func_t, )


void ucp_proto_common_add_ppln_range(const ucp_proto_init_params_t *init_params,
                                     const ucp_proto_perf_range_t *frag_range,
                                     size_t max_length)
{
    ucp_proto_caps_t *caps = init_params->caps;
    ucp_proto_perf_range_t *ppln_range;
    double frag_overhead;

    /* Add pipelined range */
    ppln_range = &caps->ranges[caps->num_ranges++];

    /* Overhead of sending one fragment before starting the pipeline */
    frag_overhead =
            ucs_linear_func_apply(frag_range->perf[UCP_PROTO_PERF_TYPE_SINGLE],
                                  frag_range->max_length) -
            ucs_linear_func_apply(frag_range->perf[UCP_PROTO_PERF_TYPE_MULTI],
                                  frag_range->max_length);

    ppln_range->max_length = max_length;

    /* Apply the pipelining effect when sending multiple fragments */
    ppln_range->perf[UCP_PROTO_PERF_TYPE_SINGLE] =
            ucs_linear_func_add(frag_range->perf[UCP_PROTO_PERF_TYPE_MULTI],
                                ucs_linear_func_make(frag_overhead, 0));

    /* Multiple send performance is the same */
    ppln_range->perf[UCP_PROTO_PERF_TYPE_MULTI] =
            frag_range->perf[UCP_PROTO_PERF_TYPE_MULTI];

    ucs_trace("frag-size: %zd" UCP_PROTO_TIME_FMT(frag_overhead),
              frag_range->max_length, UCP_PROTO_TIME_ARG(frag_overhead));
}

void ucp_proto_common_init_base_caps(
        const ucp_proto_common_init_params_t *params, size_t min_length)
{
    ucp_proto_caps_t *caps = params->super.caps;

    caps->cfg_thresh   = params->cfg_thresh;
    caps->cfg_priority = params->cfg_priority;
    caps->min_length   = ucs_max(params->min_length, min_length);
    caps->num_ranges   = 0;
}

ucs_status_t
ucp_proto_perf_envelope_make(const ucp_proto_perf_list_t *perf_list,
                             size_t range_start, size_t range_end, int convex,
                             ucp_proto_perf_envelope_t *envelope_list)
{
    const ucs_linear_func_t *perf_list_ptr = ucs_array_begin(perf_list);
    const unsigned perf_list_length        = ucs_array_length(perf_list);
    size_t start                           = range_start;
    char num_str[64];
    struct {
        unsigned index;
        double   result;
    } curr, best;
    ucp_proto_perf_envelope_elem_t *new_elem;
    ucs_status_t status;
    size_t midpoint;
    double x_sample, x_intersect;
    uint64_t mask;

    ucs_assert_always(perf_list_length < 64);
    mask = UCS_MASK(perf_list_length);

    do {
        ucs_assert(mask != 0);

        /* Find best trend at the 'start' point */
        best.index  = UINT_MAX;
        best.result = DBL_MAX;
        ucs_for_each_bit(curr.index, mask) {
            x_sample    = start + UCP_PROTO_MSGLEN_EPSILON;
            curr.result = ucs_linear_func_apply(perf_list_ptr[curr.index],
                                                x_sample);
            ucs_assert(curr.result != DBL_MAX);
            if ((best.index == UINT_MAX) ||
                ((curr.result < best.result) == convex)) {
                best = curr;
            }
        }

        /* Since mask != 0, we should find at least one trend */
        ucs_assert(best.index != UINT_MAX);
        ucs_trace("at %s: selected stage[%d]",
                  ucs_memunits_to_str(start, num_str, sizeof(num_str)),
                  best.index);
        ucs_log_indent(1);

        /* Find first (smallest) intersection point between the current best
         * trend and any other trend. This would be the point where that
         * other trend becomes the best one.
         */
        midpoint = range_end;
        mask    &= ~UCS_BIT(best.index);
        ucs_for_each_bit(curr.index, mask) {
            status = ucs_linear_func_intersect(perf_list_ptr[curr.index],
                                               perf_list_ptr[best.index],
                                               &x_intersect);
            if ((status == UCS_OK) && (x_intersect > start)) {
                /* We care only if the intersection is after 'start', since
                 * otherwise 'best' is better than 'curr' at
                 * 'end' as well as at 'start'.
                 */
                midpoint = ucs_min(ucs_double_to_sizet(x_intersect, SIZE_MAX),
                                   midpoint);
                ucs_memunits_to_str(midpoint, num_str, sizeof(num_str));
                ucs_trace("intersects with stage[%d] at %.2f, midpoint is %s",
                          curr.index, x_intersect, num_str);
            } else {
                ucs_trace("intersects with stage[%d] out of range", curr.index);
            }
        }
        ucs_log_indent(-1);

        new_elem             = ucs_array_append(ucp_proto_perf_envelope,
                                                envelope_list,
                                                return UCS_ERR_NO_MEMORY);
        new_elem->index      = best.index;
        new_elem->max_length = midpoint;

        start = midpoint + 1;
    } while (midpoint < range_end);

    return UCS_OK;
}

ucs_status_t
ucp_proto_init_parallel_stages(const ucp_proto_init_params_t *params,
                               size_t range_start, size_t range_end,
                               size_t frag_size, double bias,
                               const ucp_proto_perf_range_t **stages,
                               unsigned num_stages)
{
    ucp_proto_caps_t *caps      = params->caps;
    ucs_linear_func_t bias_func = ucs_linear_func_make(0.0, 1.0 - bias);
    UCS_ARRAY_DEFINE_ONSTACK(stage_list, ucp_proto_perf_list, 4);
    UCS_ARRAY_DEFINE_ONSTACK(concave, ucp_proto_perf_envelope, 4);
    const ucs_linear_func_t *single_perf, *multi_perf;
    const ucp_proto_perf_range_t **stage_elem;
    ucp_proto_perf_envelope_elem_t *elem;
    ucp_proto_perf_range_t *range;
    ucs_linear_func_t *perf_elem;
    ucs_linear_func_t sum_perf;
    char frag_size_str[64];
    ucs_status_t status;
    char range_str[64];

    ucs_memunits_to_str(frag_size, frag_size_str, sizeof(frag_size_str));
    ucs_trace("%s frag %s bias %.0f%%",
              ucs_memunits_range_str(range_start, range_end, range_str,
                                     sizeof(range_str)),
              frag_size_str, bias * 100.0);

    ucs_log_indent(1);
    sum_perf = UCS_LINEAR_FUNC_ZERO;
    ucs_carray_for_each(stage_elem, stages, num_stages) {
        /* Single-fragment is adding overheads and transfer time */
        single_perf = &(*stage_elem)->perf[UCP_PROTO_PERF_TYPE_SINGLE];
        ucs_linear_func_add_inplace(&sum_perf, *single_perf);

        /* account for the overhead of each fragment of a multi-fragment message */
        multi_perf   = &(*stage_elem)->perf[UCP_PROTO_PERF_TYPE_MULTI];
        perf_elem    = ucs_array_append(ucp_proto_perf_list, &stage_list,
                                        status = UCS_ERR_NO_MEMORY; goto out);
        perf_elem->c = multi_perf->c;
        perf_elem->m = multi_perf->m + (multi_perf->c / frag_size);

        ucs_trace("stage[%zu]" UCP_PROTO_PERF_FUNC_TYPES_FMT
                  UCP_PROTO_PERF_FUNC_FMT(perf_elem), stage_elem - stages,
                  UCP_PROTO_PERF_FUNC_TYPES_ARG((*stage_elem)->perf),
                  UCP_PROTO_PERF_FUNC_ARG(perf_elem));
    }

    /* Multi-fragment is pipelining overheads and network transfer */
    status = ucp_proto_perf_envelope_make(&stage_list, range_start, range_end,
                                          0, &concave);
    if (status != UCS_OK) {
        goto out;
    }

    ucs_array_for_each(elem, &concave) {
        range             = &caps->ranges[caps->num_ranges++];
        range->max_length = elem->max_length;

        /* "single" performance estimation is sum of "stages" with the bias */
        range->perf[UCP_PROTO_PERF_TYPE_SINGLE] =
                ucs_linear_func_compose(bias_func, sum_perf);

        /* "multiple" performance estimation is concave envelope of "stages" */
        multi_perf = &ucs_array_elem(&stage_list, elem->index);
        range->perf[UCP_PROTO_PERF_TYPE_MULTI] =
                ucs_linear_func_compose(bias_func, *multi_perf);

        ucs_trace("range[%d] %s" UCP_PROTO_PERF_FUNC_TYPES_FMT,
                  caps->num_ranges,
                  ucs_memunits_range_str(range_start, range->max_length,
                                         range_str, sizeof(range_str)),
                  UCP_PROTO_PERF_FUNC_TYPES_ARG(range->perf));

        range_start = range->max_length + 1;
    }
    ucs_assertv(range_start == (range_end + 1), "range_start=%zu range_end=%zu",
                range_start, range_end);

    status = UCS_OK;

out:
    ucs_log_indent(-1);
    return status;
}

ucs_status_t
ucp_proto_common_init_caps(const ucp_proto_common_init_params_t *params,
                           const ucp_proto_common_tl_perf_t *perf,
                           ucp_md_map_t reg_md_map)
{
    const ucp_proto_select_param_t *select_param = params->super.select_param;
    ucp_proto_caps_t *caps                       = params->super.caps;
    ucp_proto_perf_range_t send_perf, xfer_perf, recv_perf;
    ucs_linear_func_t send_overhead, xfer_time, recv_overhead;
    const ucp_proto_perf_range_t *parallel_stages[3];
    ucs_memory_type_t recv_mem_type;
    uint32_t op_attr_mask;
    ucs_status_t status;
    size_t frag_size;

    ucs_trace("caps" UCP_PROTO_TIME_FMT(send_pre_overhead)
              UCP_PROTO_TIME_FMT(send_post_overhead)
              UCP_PROTO_TIME_FMT(recv_overhead) UCP_PROTO_TIME_FMT(latency),
              UCP_PROTO_TIME_ARG(perf->send_pre_overhead),
              UCP_PROTO_TIME_ARG(perf->send_post_overhead),
              UCP_PROTO_TIME_ARG(perf->recv_overhead),
              UCP_PROTO_TIME_ARG(perf->latency));

    /* Remote access implies zero copy on receiver */
    if (params->flags & UCP_PROTO_COMMON_INIT_FLAG_REMOTE_ACCESS) {
        ucs_assert(params->flags & UCP_PROTO_COMMON_INIT_FLAG_RECV_ZCOPY);
    }

    op_attr_mask = ucp_proto_select_op_attr_from_flags(select_param->op_flags);

    /* Calculate sender overhead */
    if (params->flags & UCP_PROTO_COMMON_INIT_FLAG_SEND_ZCOPY) {
        send_overhead = ucp_proto_common_memreg_time(params, reg_md_map);
    } else if (params->flags & UCP_PROTO_COMMON_INIT_FLAG_RKEY_PTR) {
        send_overhead = UCS_LINEAR_FUNC_ZERO;
    } else {
        ucs_assert(reg_md_map == 0);
        status = ucp_proto_common_buffer_copy_time(
                params->super.worker, "send-copy", UCS_MEMORY_TYPE_HOST,
                select_param->mem_type, params->memtype_op, &send_overhead);
        if (status != UCS_OK) {
            return status;
        }
    }

    /* Add constant CPU overhead */
    send_overhead.c += perf->send_pre_overhead;

    send_perf.perf[UCP_PROTO_PERF_TYPE_SINGLE]   = send_overhead;
    send_perf.perf[UCP_PROTO_PERF_TYPE_MULTI]    = send_overhead;
    send_perf.perf[UCP_PROTO_PERF_TYPE_MULTI].c += perf->send_post_overhead;

    /* Calculate transport time */
    if ((op_attr_mask & UCP_OP_ATTR_FLAG_FAST_CMPL) &&
        !(params->flags & UCP_PROTO_COMMON_INIT_FLAG_SEND_ZCOPY)) {
        /* If we care only about time to start sending the message, ignore
           the transport time */
        xfer_time = UCS_LINEAR_FUNC_ZERO;
    } else {
        xfer_time = ucs_linear_func_make(0, 1.0 / perf->bandwidth);
    }

    xfer_perf.perf[UCP_PROTO_PERF_TYPE_SINGLE]    = xfer_time;
    xfer_perf.perf[UCP_PROTO_PERF_TYPE_SINGLE].c += perf->latency +
                                               perf->sys_latency;
    xfer_perf.perf[UCP_PROTO_PERF_TYPE_MULTI]     = xfer_time;

    /*
     * Add the latency of response/ACK back from the receiver.
     */
    if (/* Protocol is waiting for response */
        (params->flags & UCP_PROTO_COMMON_INIT_FLAG_RESPONSE) ||
        /* Send time is representing request completion, which in case of zcopy
           waits for ACK from remote side. */
        ((op_attr_mask & UCP_OP_ATTR_FLAG_FAST_CMPL) &&
         (params->flags & UCP_PROTO_COMMON_INIT_FLAG_SEND_ZCOPY))) {
        xfer_perf.perf[UCP_PROTO_PERF_TYPE_SINGLE].c += perf->latency;
        send_perf.perf[UCP_PROTO_PERF_TYPE_SINGLE].c += perf->send_post_overhead;
    }

    /* Calculate receiver overhead */
    if (/* Don't care about receiver time for one-sided remote access */
        (params->flags & UCP_PROTO_COMMON_INIT_FLAG_REMOTE_ACCESS) ||
        /* Count only send completion time without waiting for a response */
        ((op_attr_mask & UCP_OP_ATTR_FLAG_FAST_CMPL) &&
         !(params->flags & UCP_PROTO_COMMON_INIT_FLAG_RESPONSE))) {
        recv_overhead = UCS_LINEAR_FUNC_ZERO;
    } else {
        if (params->flags & UCP_PROTO_COMMON_INIT_FLAG_RECV_ZCOPY) {
            /* Receiver has to register its buffer */
            recv_overhead = ucp_proto_common_memreg_time(params, reg_md_map);
        } else {
            if (params->super.rkey_config_key == NULL) {
                /* Assume same memory type as sender */
                recv_mem_type = select_param->mem_type;
            } else {
                recv_mem_type = params->super.rkey_config_key->mem_type;
            }

            /* Receiver has to copy data */
            recv_overhead = UCS_LINEAR_FUNC_ZERO; /* silence cppcheck */
            ucp_proto_common_buffer_copy_time(params->super.worker, "recv-copy",
                                              UCS_MEMORY_TYPE_HOST,
                                              recv_mem_type,
                                              UCT_EP_OP_PUT_SHORT,
                                              &recv_overhead);
        }

        /* Receiver has to process the incoming message */
        if (!(params->flags & UCP_PROTO_COMMON_INIT_FLAG_REMOTE_ACCESS)) {
            /* latency measure: add remote-side processing time */
            recv_overhead.c += perf->recv_overhead;
        }
    }

    recv_perf.perf[UCP_PROTO_PERF_TYPE_SINGLE] = recv_overhead;
    recv_perf.perf[UCP_PROTO_PERF_TYPE_MULTI]  = recv_overhead;

    /* Get fragment size */
    ucs_assert(perf->max_frag >= params->hdr_size);
    frag_size = ucs_min(params->max_length, perf->max_frag - params->hdr_size);

    /* Initialize capabilities */
    ucp_proto_common_init_base_caps(params, perf->min_length);

    parallel_stages[0] = &send_perf;
    parallel_stages[1] = &xfer_perf;
    parallel_stages[2] = &recv_perf;

    /* Add ranges representing sending single fragment */
    status = ucp_proto_init_parallel_stages(&params->super, 0, frag_size,
                                            frag_size, 0.0, parallel_stages, 3);
    if (status != UCS_OK) {
        return status;
    }

    /* Append range representing sending rest of the fragments, if frag_size is
       not the max length and the protocol supports fragmentation */
    if ((frag_size < params->max_length) &&
        !(params->flags & UCP_PROTO_COMMON_INIT_FLAG_SINGLE_FRAG)) {
        ucp_proto_common_add_ppln_range(&params->super,
                                        &caps->ranges[caps->num_ranges - 1],
                                        params->max_length);
    }

    return UCS_OK;
}
