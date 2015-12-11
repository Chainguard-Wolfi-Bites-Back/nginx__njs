
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) NGINX, Inc.
 */

#include <nxt_types.h>
#include <nxt_clang.h>
#include <nxt_alignment.h>
#include <nxt_stub.h>
#include <nxt_djb_hash.h>
#include <nxt_array.h>
#include <nxt_lvlhsh.h>
#include <nxt_mem_cache_pool.h>
#include <njscript.h>
#include <njs_vm.h>
#include <njs_number.h>
#include <njs_string.h>
#include <njs_object.h>
#include <njs_object_hash.h>
#include <njs_array.h>
#include <njs_function.h>
#include <njs_regexp.h>
#include <njs_extern.h>
#include <njs_variable.h>
#include <njs_parser.h>
#include <string.h>


/* The values must be greater than NXT_OK. */
#define NJS_PRIMITIVE_VALUE        1
#define NJS_STRING_VALUE           2
#define NJS_ARRAY_VALUE            3
#define NJS_EXTERNAL_VALUE         4


/*
 * NJS_PROPERTY_QUERY_GET must be less or equal to NJS_PROPERTY_QUERY_IN,
 * NJS_PROPERTY_QUERY_SET and NJS_PROPERTY_QUERY_DELETE must be greater
 * than NJS_PROPERTY_QUERY_IN.
 */
#define NJS_PROPERTY_QUERY_GET     0
#define NJS_PROPERTY_QUERY_IN      1
#define NJS_PROPERTY_QUERY_SET     2
#define NJS_PROPERTY_QUERY_DELETE  3


typedef struct {
    nxt_lvlhsh_query_t             lhq;
    njs_value_t                    value;
    njs_object_t                   *prototype;
    uint8_t                        query;
    uint8_t                        shared;
} njs_property_query_t;


typedef struct {
    int32_t                        index;
    nxt_lvlhsh_each_t              lhe;
} njs_property_each_t;


/*
 * These functions are forbidden to inline to minimize JavaScript VM
 * interpreter memory footprint.  The size is less than 8K on AMD64
 * and should fit in CPU L1 instruction cache.
 */

static nxt_noinline njs_ret_t njs_property_query(njs_vm_t *vm,
    njs_property_query_t *pq, njs_value_t *object, njs_value_t *property);
static njs_ret_t njs_array_property_query(njs_vm_t *vm,
    njs_property_query_t *pq, njs_value_t *object, int32_t index);
static njs_ret_t njs_object_property_query(njs_vm_t *vm,
    njs_property_query_t *pq, njs_value_t *value, njs_object_t *object);
static njs_ret_t njs_function_private_copy(njs_vm_t *vm,
    njs_property_query_t *pq);
static nxt_noinline uint32_t njs_integer_value(double num);
static nxt_noinline njs_ret_t njs_values_equal(njs_value_t *val1,
    njs_value_t *val2);
static nxt_noinline njs_ret_t njs_values_compare(njs_value_t *val1,
    njs_value_t *val2);
static nxt_noinline nxt_bool_t njs_values_strict_equal(njs_value_t *val1,
    njs_value_t *val2);
static nxt_noinline njs_ret_t njs_function_frame_free(njs_vm_t *vm,
    njs_native_frame_t *frame, njs_native_frame_t *skip);

static njs_ret_t njs_vmcode_number_primitive(njs_vm_t *vm, njs_value_t *invld,
    njs_value_t *narg);
static njs_ret_t njs_vmcode_string_primitive(njs_vm_t *vm, njs_value_t *invld,
    njs_value_t *narg);
static njs_ret_t njs_primitive_value(njs_vm_t *vm, njs_value_t *value,
    nxt_uint_t hint);
static njs_ret_t njs_vm_trap(njs_vm_t *vm, nxt_uint_t trap, njs_value_t *value1,
    njs_value_t *value2);
static njs_ret_t njs_vmcode_restart(njs_vm_t *vm, njs_value_t *invld1,
    njs_value_t *invld2);

void njs_debug(njs_index_t index, njs_value_t *value);


const njs_value_t  njs_value_null =         njs_value(NJS_NULL, 0, 0.0);
const njs_value_t  njs_value_void =         njs_value(NJS_VOID, 0, NJS_NAN);
const njs_value_t  njs_value_false =        njs_value(NJS_BOOLEAN, 0, 0.0);
const njs_value_t  njs_value_true =         njs_value(NJS_BOOLEAN, 1, 1.0);
const njs_value_t  njs_value_zero =         njs_value(NJS_NUMBER, 0, 0.0);
const njs_value_t  njs_value_nan =          njs_value(NJS_NUMBER, 0, NJS_NAN);

const njs_value_t  njs_string_empty =       njs_string("");
const njs_value_t  njs_string_comma =       njs_string(",");
const njs_value_t  njs_string_null =        njs_string("null");
const njs_value_t  njs_string_void =        njs_string("undefined");
const njs_value_t  njs_string_boolean =     njs_string("boolean");
const njs_value_t  njs_string_false =       njs_string("false");
const njs_value_t  njs_string_true =        njs_string("true");
const njs_value_t  njs_string_number =      njs_string("number");
const njs_value_t  njs_string_minus_infinity =
                                            njs_string("-Infinity");
const njs_value_t  njs_string_plus_infinity =
                                            njs_string("Infinity");
const njs_value_t  njs_string_nan =         njs_string("NaN");
const njs_value_t  njs_string_string =      njs_string("string");
const njs_value_t  njs_string_object =      njs_string("object");
const njs_value_t  njs_string_function =    njs_string("function");
const njs_value_t  njs_string_native =      njs_string("[native code]");

const njs_value_t  njs_exception_syntax_error =    njs_string("SyntaxError");
const njs_value_t  njs_exception_reference_error = njs_string("ReferenceError");
const njs_value_t  njs_exception_type_error =      njs_string("TypeError");
const njs_value_t  njs_exception_range_error =     njs_string("RangeError");
const njs_value_t  njs_exception_memory_error =    njs_string("MemoryError");


/*
 * The nJSVM is optimized for an ABIs where the first several arguments
 * are passed in registers (AMD64, ARM32/64): two pointers to the operand
 * values is passed as arguments although they are not always used.
 */

nxt_int_t
njs_vmcode_interpreter(njs_vm_t *vm)
{
    u_char                *catch;
    njs_ret_t             ret;
    njs_value_t           *retval, *value1, *value2;
    njs_frame_t           *frame;
    njs_native_frame_t    *previous;
    njs_vmcode_generic_t  *vmcode;

    for ( ;; ) {

    again:
        for ( ;; ) {

            vmcode = (njs_vmcode_generic_t *) vm->current;

            /*
             * The first operand is passed as is in value2 to
             *   njs_vmcode_jump(),
             *   njs_vmcode_if_true_jump(),
             *   njs_vmcode_if_false_jump(),
             *   njs_vmcode_validate(),
             *   njs_vmcode_function_frame(),
             *   njs_vmcode_method_frame(),
             *   njs_vmcode_function_call(),
             *   njs_vmcode_return(),
             *   njs_vmcode_try_start(),
             *   njs_vmcode_try_next(),
             *   njs_vmcode_try_end(),
             *   njs_vmcode_catch().
             *   njs_vmcode_throw().
             *   njs_vmcode_stop().
             */
            value2 = (njs_value_t *) vmcode->operand1;
            value1 = NULL;

            switch (vmcode->code.operands) {

            case NJS_VMCODE_3OPERANDS:
                value2 = njs_vmcode_operand(vm, vmcode->operand3);

            case NJS_VMCODE_2OPERANDS:
                value1 = njs_vmcode_operand(vm, vmcode->operand2);
            }

            ret = vmcode->code.operation(vm, value1, value2);

            /*
             * On success an operation returns size of the bytecode,
             * a jump offset or zero after the call or return operations.
             * Jumps can return a negative offset.  Compilers can generate
             *    (ret < 0 && ret >= NJS_PASS)
             * as a single unsigned comparision.
             */

            if (nxt_slow_path(ret < 0 && ret >= NJS_PASS)) {
                break;
            }

            vm->current += ret;

            if (vmcode->code.retval) {
                retval = njs_vmcode_operand(vm, vmcode->operand1);
                //njs_release(vm, retval);
                *retval = vm->retval;
            }
        }

        switch (ret) {

        case NJS_TRAP_NUMBER:
            value2 = value1;

            /* Fall through. */

        case NJS_TRAP_NUMBERS:
        case NJS_TRAP_STRINGS:
        case NJS_TRAP_INCDEC:
        case NJS_TRAP_PROPERTY:

            ret = njs_vm_trap(vm, ret - NJS_TRAP_LAST, value1, value2);

            if (nxt_fast_path(ret == NXT_OK)) {
                goto again;
            }

            ret = NXT_ERROR;
            break;

        default:
            break;
        }

        if (ret == NXT_ERROR) {

            for ( ;; ) {
                frame = (njs_frame_t *) vm->frame;
                catch = frame->native.exception.catch;

                if (catch != NULL) {
                    vm->current = catch;
                    goto again;
                }

                previous = frame->native.previous;
                if (previous == NULL) {
                    return ret;
                }

                vm->frame = previous;

                /* GC: NJS_SCOPE_ARGUMENTS and NJS_SCOPE_LOCAL. */

                vm->scopes[NJS_SCOPE_CALLEE_ARGUMENTS] = previous->arguments;
                vm->scopes[NJS_SCOPE_LOCAL] = frame->prev_local;
                vm->scopes[NJS_SCOPE_ARGUMENTS] = frame->prev_arguments;

                if (frame->native.first) {
                    nxt_mem_cache_free(vm->mem_cache_pool, frame);
                }
            }
        }

        /* NXT_AGAIN, NXT_DONE */

        return ret;
    }
}


nxt_noinline void
njs_value_retain(njs_value_t *value)
{
    njs_string_t  *string;

    if (value->type == NJS_STRING) {

        if (value->data.external0 != 0xff) {
            string = value->data.u.string;

            nxt_thread_log_debug("retain:%uxD \"%*s\"", string->retain,
                                 value->data.string_size, string->start);

            if (string->retain != 0xffff) {
                string->retain++;
            }
        }
    }
}


nxt_noinline void
njs_value_release(njs_vm_t *vm, njs_value_t *value)
{
    njs_string_t  *string;

    if (value->type == NJS_STRING) {

        if (value->data.external0 != 0xff) {
            string = value->data.u.string;

            nxt_thread_log_debug("release:%uxD \"%*s\"", string->retain,
                                 value->data.string_size, string->start);

            if (string->retain != 0xffff) {
                string->retain--;

#if 0
                if (string->retain == 0) {
                    if ((u_char *) string + sizeof(njs_string_t)
                        != string->start)
                    {
                        nxt_memcache_pool_free(vm->mem_cache_pool,
                                               string->start);
                    }

                    nxt_memcache_pool_free(vm->mem_cache_pool, string);
                }
#endif
            }
        }
    }
}


njs_ret_t
njs_vmcode_object(njs_vm_t *vm, njs_value_t *invld1, njs_value_t *invld2)
{
    njs_object_t  *object;

    object = njs_object_alloc(vm);

    if (nxt_fast_path(object != NULL)) {
        vm->retval.data.u.object = object;
        vm->retval.type = NJS_OBJECT;
        vm->retval.data.truth = 1;

        return sizeof(njs_vmcode_object_t);
    }

    return NXT_ERROR;
}


njs_ret_t
njs_vmcode_array(njs_vm_t *vm, njs_value_t *invld1, njs_value_t *invld2)
{
    uint32_t            size;
    njs_array_t         *array;
    njs_value_t         *value;
    njs_vmcode_array_t  *code;

    code = (njs_vmcode_array_t *) vm->current;

    array = njs_array_alloc(vm, code->length, NJS_ARRAY_SPARE);

    if (nxt_fast_path(array != NULL)) {
        size = array->size;
        value = array->start;

        do {
            njs_set_invalid(value);
            value++;
            size--;
        } while (size != 0);

        vm->retval.data.u.array = array;
        vm->retval.type = NJS_ARRAY;
        vm->retval.data.truth = 1;

        return sizeof(njs_vmcode_array_t);
    }

    return NXT_ERROR;
}


njs_ret_t
njs_vmcode_function(njs_vm_t *vm, njs_value_t *invld1,
    njs_value_t *invld2)
{
    njs_function_t         *function;
    njs_vmcode_function_t  *code;

    function = nxt_mem_cache_zalloc(vm->mem_cache_pool, sizeof(njs_function_t));

    if (nxt_fast_path(function != NULL)) {
        function->object.__proto__ = &vm->prototypes[NJS_PROTOTYPE_FUNCTION];
        function->args_offset = 1;

        code = (njs_vmcode_function_t *) vm->current;
        function->u.lambda = code->lambda;
        vm->retval.data.u.function = function;
        vm->retval.type = NJS_FUNCTION;
        vm->retval.data.truth = 1;

        return sizeof(njs_vmcode_function_t);
    }

    return NXT_ERROR;
}


njs_ret_t
njs_vmcode_regexp(njs_vm_t *vm, njs_value_t *invld1, njs_value_t *invld2)
{
    njs_regexp_t         *regexp;
    njs_vmcode_regexp_t  *code;

    code = (njs_vmcode_regexp_t *) vm->current;

    regexp = njs_regexp_alloc(vm, code->pattern);

    if (nxt_fast_path(regexp != NULL)) {
        vm->retval.data.u.regexp = regexp;
        vm->retval.type = NJS_REGEXP;
        vm->retval.data.truth = 1;

        return sizeof(njs_vmcode_regexp_t);
    }

    return NXT_ERROR;
}


njs_ret_t
njs_vmcode_property_get(njs_vm_t *vm, njs_value_t *object,
    njs_value_t *property)
{
    double                num;
    int32_t               index;
    uintptr_t             data;
    njs_ret_t             ret;
    njs_value_t           *val;
    njs_extern_t          *ext;
    njs_slice_prop_t      slice;
    njs_string_prop_t     string;
    njs_object_prop_t     *prop;
    const njs_value_t     *retval;
    njs_property_query_t  pq;

    pq.query = NJS_PROPERTY_QUERY_GET;

    ret = njs_property_query(vm, &pq, object, property);

    retval = &njs_value_void;

    switch (ret) {

    case NXT_OK:
        prop = pq.lhq.value;

        switch (prop->type) {

        case NJS_METHOD:
            if (pq.shared) {
                ret = njs_function_private_copy(vm, &pq);

                if (nxt_slow_path(ret != NXT_OK)) {
                    return ret;
                }

                prop = pq.lhq.value;
            }

        case NJS_PROPERTY:
            retval = &prop->value;
            break;

        case NJS_NATIVE_GETTER:
            ret = prop->value.data.u.getter(vm, object);

            if (nxt_fast_path(ret == NXT_OK)) {
                return sizeof(njs_vmcode_prop_get_t);
            }

            return ret;

        default:
            nxt_thread_log_alert("invalid property get type:%d", prop->type);

            return NXT_ERROR;
        }

        break;

    case NXT_DECLINED:
    case NJS_PRIMITIVE_VALUE:
        break;

    case NJS_STRING_VALUE:

        /* string[n]. */

        num = njs_value_to_number(property);
        index = (int32_t) num;

        if (index >= 0 && index == num) {
            slice.start = index;
            slice.length = 1;
            slice.string_length = njs_string_prop(&string, object);
            /*
             * A single codepoint string fits in vm->retval
             * so the function cannot fail.
             */
            (void) njs_string_slice(vm, &vm->retval, &string, &slice);

            if (nxt_fast_path(vm->retval.data.truth != 0)) {
                /* Non-empty string. */
                return sizeof(njs_vmcode_prop_get_t);
            }
        }

        break;

    case NJS_ARRAY_VALUE:
        val = pq.lhq.value;

        if (njs_is_valid(val)) {
            retval = val;
        }

        break;

    case NJS_EXTERNAL_VALUE:
        ext = object->data.u.external;

        ret = nxt_lvlhsh_find(&ext->hash, &pq.lhq);

        if (ret == NXT_OK) {
            ext = pq.lhq.value;

            if ((ext->type & NJS_EXTERN_OBJECT) != 0) {
                retval = &ext->value;
                break;
            }

            data = ext->data;

        } else {
            data = (uintptr_t) &pq.lhq.key;
        }

        vm->retval = njs_value_void;

        ret = ext->get(vm, &vm->retval, vm->external[ext->object], data);
        if (nxt_slow_path(ret != NXT_OK)) {
            return ret;
        }

        /* The vm->retval is already retained by ext->get(). */

        return sizeof(njs_vmcode_prop_get_t);

    default:
        /* NJS_TRAP_PROPERTY */
        /* NXT_ERROR */

        return ret;
    }

    vm->retval = *retval;

    /* GC: njs_retain(retval) */

    return sizeof(njs_vmcode_prop_get_t);
}


njs_ret_t
njs_vmcode_property_set(njs_vm_t *vm, njs_value_t *object,
    njs_value_t *property)
{
    uintptr_t              data;
    nxt_str_t              s;
    njs_ret_t              ret;
    njs_value_t            *p, *value;
    njs_extern_t           *ext;
    njs_object_prop_t      *prop;
    njs_property_query_t   pq;
    njs_vmcode_prop_set_t  *code;

    code = (njs_vmcode_prop_set_t *) vm->current;
    value = njs_vmcode_operand(vm, code->value);

    pq.query = NJS_PROPERTY_QUERY_SET;

    ret = njs_property_query(vm, &pq, object, property);

    switch (ret) {

    case NXT_OK:
        prop = pq.lhq.value;
        break;

    case NXT_DECLINED:
        prop = njs_object_prop_alloc(vm, &pq.value);
        if (nxt_slow_path(prop == NULL)) {
            return NXT_ERROR;
        }

        pq.lhq.replace = 0;
        pq.lhq.value = prop;
        pq.lhq.pool = vm->mem_cache_pool;

        ret = nxt_lvlhsh_insert(&object->data.u.object->hash, &pq.lhq);
        if (nxt_slow_path(ret != NXT_OK)) {
            /* Only NXT_ERROR can be returned here. */
            return ret;
        }

        break;

    case NJS_PRIMITIVE_VALUE:
    case NJS_STRING_VALUE:
        return sizeof(njs_vmcode_prop_set_t);

    case NJS_ARRAY_VALUE:
        p = pq.lhq.value;
        *p = *value;

        return sizeof(njs_vmcode_prop_set_t);

    case NJS_EXTERNAL_VALUE:
        ext = object->data.u.external;

        ret = nxt_lvlhsh_find(&ext->hash, &pq.lhq);

        if (ret == NXT_OK) {
            ext = pq.lhq.value;
            data = ext->data;

        } else {
            data = (uintptr_t) &pq.lhq.key;
        }

        ret = njs_value_to_ext_string(vm, &s, value);
        if (nxt_slow_path(ret != NXT_OK)) {
            return ret;
        }

        /* TODO retain value if it is string. */

        ret = ext->set(vm, vm->external[ext->object], data, &s);
        if (nxt_slow_path(ret != NXT_OK)) {
            return ret;
        }

        return sizeof(njs_vmcode_prop_set_t);

    default:
        /* NJS_TRAP_PROPERTY */
        /* NXT_ERROR */

        return ret;
    }

    prop->value = *value;

    return sizeof(njs_vmcode_prop_set_t);
}


njs_ret_t
njs_vmcode_property_in(njs_vm_t *vm, njs_value_t *object, njs_value_t *property)
{
    uintptr_t             data;
    njs_ret_t             ret;
    njs_value_t           *value;
    njs_extern_t          *ext;
    const njs_value_t     *retval;
    njs_property_query_t  pq;

    retval = &njs_value_false;

    pq.query = NJS_PROPERTY_QUERY_IN;

    ret = njs_property_query(vm, &pq, object, property);

    switch (ret) {

    case NXT_OK:
        retval = &njs_value_true;
        break;

    case NXT_DECLINED:
        break;

    case NJS_PRIMITIVE_VALUE:
    case NJS_STRING_VALUE:
        vm->exception = &njs_exception_type_error;

        return NXT_ERROR;

    case NJS_ARRAY_VALUE:
        value = pq.lhq.value;

        if (njs_is_valid(value)) {
            retval = &njs_value_true;
        }

        break;

    case NJS_EXTERNAL_VALUE:
        ext = object->data.u.external;

        ret = nxt_lvlhsh_find(&ext->hash, &pq.lhq);

        if (ret == NXT_OK) {
            ext = pq.lhq.value;

            if ((ext->type & NJS_EXTERN_OBJECT) != 0) {
                retval = &njs_value_true;
                break;
            }

            data = ext->data;

        } else {
            data = (uintptr_t) &pq.lhq.key;
        }

        ret = ext->find(vm, vm->external[ext->object], data, 0);

        if (nxt_slow_path(ret == NXT_ERROR)) {
            return ret;
        }

        if (ret == NXT_OK) {
            retval = &njs_value_true;
        }

        break;

    default:
        /* NJS_TRAP_PROPERTY */
        /* NXT_ERROR */

        return ret;
    }

    vm->retval = *retval;

    return sizeof(njs_vmcode_3addr_t);
}


njs_ret_t
njs_vmcode_property_delete(njs_vm_t *vm, njs_value_t *object,
    njs_value_t *property)
{
    uintptr_t             data;
    njs_ret_t             ret;
    njs_value_t           *value;
    njs_extern_t          *ext;
    const njs_value_t     *retval;
    njs_object_prop_t     *prop;
    njs_property_query_t  pq;

    retval = &njs_value_false;

    pq.query = NJS_PROPERTY_QUERY_DELETE;

    ret = njs_property_query(vm, &pq, object, property);

    switch (ret) {

    case NXT_OK:
        prop = pq.lhq.value;

        if (prop->configurable) {
            pq.lhq.pool = vm->mem_cache_pool;

            (void) nxt_lvlhsh_delete(&object->data.u.object->hash, &pq.lhq);

            njs_release(vm, property);

            retval = &njs_value_true;
        }

        break;

    case NXT_DECLINED:
    case NJS_PRIMITIVE_VALUE:
    case NJS_STRING_VALUE:
        break;

    case NJS_ARRAY_VALUE:
        value = pq.lhq.value;
        njs_set_invalid(value);
        retval = &njs_value_true;
        break;

    case NJS_EXTERNAL_VALUE:

        ext = object->data.u.external;

        ret = nxt_lvlhsh_find(&ext->hash, &pq.lhq);

        if (ret == NXT_OK) {
            ext = pq.lhq.value;

            if ((ext->type & NJS_EXTERN_OBJECT) != 0) {
                break;
            }

            data = ext->data;

        } else {
            data = (uintptr_t) &pq.lhq.key;
        }

        ret = ext->find(vm, vm->external[ext->object], data, 1);

        if (nxt_slow_path(ret == NXT_ERROR)) {
            return ret;
        }

        if (ret == NXT_OK) {
            retval = &njs_value_true;
        }

        break;

    default:
        /* NJS_TRAP_PROPERTY */
        /* NXT_ERROR */

        return ret;
    }

    vm->retval = *retval;

    return sizeof(njs_vmcode_3addr_t);
}


/*
 * The njs_property_query() returns values
 *   NXT_OK               property has been found in object,
 *   NXT_DECLINED         property was not found in object,
 *   NJS_PRIMITIVE_VALUE  property operation was applied to a numeric
 *                        or boolean value,
 *   NJS_STRING_VALUE     property operation was applied to a string,
 *   NJS_ARRAY_VALUE      object is array,
 *   NJS_EXTERNAL_VALUE   object is external entity,
 *   NJS_TRAP_PROPERTY    the property trap must be called,
 *   NXT_ERROR            exception has been thrown.
 */

static nxt_noinline njs_ret_t
njs_property_query(njs_vm_t *vm, njs_property_query_t *pq, njs_value_t *object,
    njs_value_t *property)
{
    double        num;
    int32_t       index;
    uint32_t      (*hash)(const void *, size_t);
    njs_ret_t     ret;
    njs_extern_t  *ext;
    njs_object_t  *obj;

    hash = nxt_djb_hash;

    switch (object->type) {

    case NJS_BOOLEAN:
    case NJS_NUMBER:
        if (pq->query != NJS_PROPERTY_QUERY_GET) {
            return NJS_PRIMITIVE_VALUE;
        }

        obj = &vm->prototypes[njs_primitive_prototype_index(object->type)];
        break;

    case NJS_STRING:
        if (pq->query == NJS_PROPERTY_QUERY_DELETE) {
            return NXT_DECLINED;
        }

        obj = &vm->prototypes[NJS_PROTOTYPE_STRING];
        break;

    case NJS_ARRAY:
        if (nxt_fast_path(!njs_is_null_or_void_or_boolean(property))) {

            if (nxt_fast_path(njs_is_primitive(property))) {
                num = njs_value_to_number(property);

            } else {
                return NJS_TRAP_PROPERTY;
            }

            index = (int) num;

            if (nxt_fast_path(index >= 0 && (double) index == num)) {
                return njs_array_property_query(vm, pq, object, index);
            }
        }

        /* Fall through. */

    case NJS_OBJECT:
    case NJS_FUNCTION:
    case NJS_REGEXP:
        obj = object->data.u.object;
        break;

    case NJS_NATIVE:
        obj = &vm->prototypes[NJS_PROTOTYPE_FUNCTION];
        break;

    case NJS_EXTERNAL:
        ext = object->data.u.external;

        if (ext->type == NJS_EXTERN_CASELESS_OBJECT) {
            hash = nxt_djb_hash_lowcase;
        }

        obj = NULL;
        break;

    default:  /* NJS_VOID, NJS_NULL. */
        vm->exception = &njs_exception_type_error;
        return NXT_ERROR;
    }

    if (nxt_fast_path(njs_is_primitive(property))) {

        ret = njs_value_to_string(vm, &pq->value, property);

        if (nxt_fast_path(ret == NXT_OK)) {

            pq->lhq.key.len = pq->value.short_string.size;

            if (pq->lhq.key.len != NJS_STRING_LONG) {
                pq->lhq.key.data = pq->value.short_string.start;

            } else {
                pq->lhq.key.len = pq->value.data.string_size;
                pq->lhq.key.data = pq->value.data.u.string->start;
            }

            pq->lhq.key_hash = hash(pq->lhq.key.data, pq->lhq.key.len);

            if (obj == NULL) {
                pq->lhq.proto = &njs_extern_hash_proto;

                return NJS_EXTERNAL_VALUE;
            }

            return njs_object_property_query(vm, pq, object, obj);
        }

        return ret;
    }

    return NJS_TRAP_PROPERTY;
}


static njs_ret_t
njs_array_property_query(njs_vm_t *vm, njs_property_query_t *pq,
    njs_value_t *object, int32_t index)
{
    njs_ret_t    ret;
    njs_array_t  *array;

    array = object->data.u.array;

    if ((uint32_t) index >= array->length) {

        if (pq->query != NJS_PROPERTY_QUERY_SET) {
            return NXT_DECLINED;
        }

        if ((uint32_t) index >= array->size) {
            ret = njs_array_realloc(vm, array, 0, index);
            if (nxt_slow_path(ret != NXT_OK)) {
                return ret;
            }
        }

        array->length = index + 1;
    }

    pq->lhq.value = &array->start[index];

    return NJS_ARRAY_VALUE;
}


static njs_ret_t
njs_object_property_query(njs_vm_t *vm, njs_property_query_t *pq,
    njs_value_t *value, njs_object_t *object)
{
    njs_ret_t          ret;
    njs_object_prop_t  *prop;

    pq->lhq.proto = &njs_object_hash_proto;

    do {
        pq->prototype = object;

        ret = nxt_lvlhsh_find(&object->hash, &pq->lhq);

        if (ret == NXT_OK) {
            prop = pq->lhq.value;

            if (prop->type != NJS_WHITEOUT) {
                pq->shared = 0;
                return ret;
            }

            goto next;
        }

        if (pq->query > NJS_PROPERTY_QUERY_IN) {
            /* NXT_DECLINED */
            return ret;
        }

        ret = nxt_lvlhsh_find(&object->shared_hash, &pq->lhq);

        if (ret == NXT_OK) {
            pq->shared = 1;

            if (pq->query == NJS_PROPERTY_QUERY_IN) {
                prop = pq->lhq.value;

                if (prop->type == NJS_WHITEOUT) {
                    return NXT_DECLINED;
                }
            }

            return ret;
        }

        if (pq->query > NJS_PROPERTY_QUERY_IN) {
            /* NXT_DECLINED */
            return ret;
        }

    next:

        object = object->__proto__;

    } while (object != NULL);

    if (njs_is_string(value)) {
        return NJS_STRING_VALUE;
    }

    /* NXT_DECLINED */

    return ret;
}


static njs_ret_t
njs_function_private_copy(njs_vm_t *vm, njs_property_query_t *pq)
{
    njs_function_t     *func;
    njs_object_prop_t  *prop, *shared;

    prop = nxt_mem_cache_alloc(vm->mem_cache_pool, sizeof(njs_object_prop_t));
    if (nxt_slow_path(prop == NULL)) {
        return NXT_ERROR;
    }

    shared = pq->lhq.value;
    *prop = *shared;

    func = nxt_mem_cache_alloc(vm->mem_cache_pool, sizeof(njs_function_t));
    if (nxt_slow_path(func == NULL)) {
        return NXT_ERROR;
    }

    *func = *prop->value.data.u.function;
    func->object.__proto__ = &vm->prototypes[NJS_PROTOTYPE_FUNCTION];
    prop->value.data.u.function = func;

    pq->lhq.replace = 0;
    pq->lhq.value = prop;
    pq->lhq.pool = vm->mem_cache_pool;

    return nxt_lvlhsh_insert(&pq->prototype->hash, &pq->lhq);
}


njs_ret_t
njs_vmcode_property_each_start(njs_vm_t *vm, njs_value_t *object,
    njs_value_t *invld)
{
    njs_ret_t                ret;
    njs_extern_t             *ext;
    njs_property_each_t      *pe;
    njs_vmcode_prop_start_t  *code;

    if (njs_is_object(object)) {
        pe = nxt_mem_cache_alloc(vm->mem_cache_pool,
                                 sizeof(njs_property_each_t));
        if (nxt_slow_path(pe == NULL)) {
            return NXT_ERROR;
        }

        vm->retval.data.u.data = pe;

        memset(&pe->lhe, 0, sizeof(nxt_lvlhsh_each_t));
        pe->lhe.proto = &njs_object_hash_proto;
        pe->index = -1;

        if (njs_is_array(object) && object->data.u.array->size != 0) {
            pe->index = 0;
        }

    } else if (njs_is_external(object)) {
        ext = object->data.u.external;

        if (ext->each_start != NULL) {
            ret = ext->each_start(vm, vm->external[ext->object], &vm->retval);
            if (nxt_slow_path(ret != NXT_OK)) {
                return ret;
            }
        }
    }

    code = (njs_vmcode_prop_start_t *) vm->current;

    return code->offset;
}


njs_ret_t
njs_vmcode_property_each(njs_vm_t *vm, njs_value_t *object, njs_value_t *each)
{
    njs_ret_t               ret;
    nxt_uint_t              n;
    njs_array_t             *array;
    njs_extern_t            *ext;
    njs_object_prop_t       *prop;
    njs_property_each_t     *pe;
    njs_vmcode_prop_each_t  *code;

    code = (njs_vmcode_prop_each_t *) vm->current;

    if (njs_is_object(object)) {
        pe = each->data.u.data;

        if (pe->index >= 0) {
            array = object->data.u.array;

            while ((uint32_t) pe->index < array->size) {
                n = pe->index++;

                if (njs_is_valid(&array->start[n])) {
                    njs_number_set(&vm->retval, n);

                    return code->offset;
                }
            }

            pe->index = -1;
        }

        prop = nxt_lvlhsh_each(&object->data.u.object->hash, &pe->lhe);

        if (prop != NULL) {
            vm->retval = prop->name;

            return code->offset;
        }

        nxt_mem_cache_free(vm->mem_cache_pool, pe);

        vm->retval = njs_value_void;

    } else if (njs_is_external(object)) {
        ext = object->data.u.external;

        if (ext->each != NULL) {
            ret = ext->each(vm, &vm->retval, vm->external[ext->object], each);

            if (ret == NXT_OK) {
                return code->offset;
            }

            if (nxt_slow_path(ret == NXT_ERROR)) {
                return ret;
            }

            /* ret == NXT_DONE. */
        }
    }

    return sizeof(njs_vmcode_prop_each_t);
}


njs_ret_t
njs_vmcode_instance_of(njs_vm_t *vm, njs_value_t *object,
    njs_value_t *constructor)
{
    nxt_int_t           ret;
    njs_value_t         *value;
    njs_object_t        *prototype, *proto;
    njs_object_prop_t   *prop;
    const njs_value_t   *retval;
    nxt_lvlhsh_query_t  lhq;

    if (!njs_is_function(constructor) && !njs_is_native(constructor)) {
        vm->exception = &njs_exception_type_error;
        return NXT_ERROR;
    }

    retval = &njs_value_false;

    if (njs_is_object(object)) {

        lhq.key_hash = NJS_PROTOTYPE_HASH;
        lhq.key.len = sizeof("prototype") - 1;
        lhq.key.data = (u_char *) "prototype";

        prop = njs_object_property(vm, constructor->data.u.object, &lhq);

        if (prop != NULL) {
            value = &prop->value;

            if (prop->type == NJS_NATIVE_GETTER) {
                /*
                 * STUB: getter should be called by some njs_object_property()
                 */
                ret = prop->value.data.u.getter(vm, constructor);

                if (nxt_slow_path(ret != NXT_OK)) {
                    return ret;
                }

                value = &vm->retval;
            }

            /* TODO: test prop->value is object. */

            prototype = value->data.u.object;
            proto = object->data.u.object;

            do {
                proto = proto->__proto__;

                if (proto == prototype) {
                    retval = &njs_value_true;
                    break;
                }

            } while (proto != NULL);
        }
    }

    vm->retval = *retval;

    return sizeof(njs_vmcode_instance_of_t);
}


/*
 * The increment and decrement operations require only one value parameter.
 * However, if the value is not numeric, then the trap is generated and
 * value parameter points to a trap frame value converted to a numeric.
 * So the additional reference parameter points to the original value.
 */

njs_ret_t
njs_vmcode_increment(njs_vm_t *vm, njs_value_t *reference, njs_value_t *value)
{
    double  num;

    if (nxt_fast_path(njs_is_numeric(value))) {
        num = value->data.u.number + 1.0;

        njs_release(vm, reference);

        njs_number_set(reference, num);
        vm->retval = *reference;

        return sizeof(njs_vmcode_3addr_t);
    }

    return NJS_TRAP_INCDEC;
}


njs_ret_t
njs_vmcode_decrement(njs_vm_t *vm, njs_value_t *reference, njs_value_t *value)
{
    double  num;

    if (nxt_fast_path(njs_is_numeric(value))) {
        num = value->data.u.number - 1.0;

        njs_release(vm, reference);

        njs_number_set(reference, num);
        vm->retval = *reference;

        return sizeof(njs_vmcode_3addr_t);
    }

    return NJS_TRAP_INCDEC;
}


njs_ret_t
njs_vmcode_post_increment(njs_vm_t *vm, njs_value_t *reference,
    njs_value_t *value)
{
    double  num;

    if (nxt_fast_path(njs_is_numeric(value))) {
        num = value->data.u.number;

        njs_release(vm, reference);

        njs_number_set(reference, num + 1.0);
        njs_number_set(&vm->retval, num);

        return sizeof(njs_vmcode_3addr_t);
    }

    return NJS_TRAP_INCDEC;
}


njs_ret_t
njs_vmcode_post_decrement(njs_vm_t *vm, njs_value_t *reference,
    njs_value_t *value)
{
    double  num;

    if (nxt_fast_path(njs_is_numeric(value))) {
        num = value->data.u.number;

        njs_release(vm, reference);

        njs_number_set(reference, num - 1.0);
        njs_number_set(&vm->retval, num);

        return sizeof(njs_vmcode_3addr_t);
    }

    return NJS_TRAP_INCDEC;
}


njs_ret_t
njs_vmcode_typeof(njs_vm_t *vm, njs_value_t *value, njs_value_t *invld)
{
    /* ECMAScript 5.1: null, array and regexp are objects. */

    static const njs_value_t  *types[] = {
        &njs_string_object,
        &njs_string_void,
        &njs_string_boolean,
        &njs_string_number,
        &njs_string_string,
        &njs_string_void,
        &njs_string_void,
        &njs_string_void,

        &njs_string_object,
        &njs_string_object,
        &njs_string_object,
        &njs_string_object,
        &njs_string_object,
        &njs_string_function,
        &njs_string_object,
    };

    vm->retval = *types[value->type];

    return sizeof(njs_vmcode_2addr_t);
}


njs_ret_t
njs_vmcode_void(njs_vm_t *vm, njs_value_t *invld1, njs_value_t *invld2)
{
    vm->retval = njs_value_void;

    return sizeof(njs_vmcode_2addr_t);
}


njs_ret_t
njs_vmcode_delete(njs_vm_t *vm, njs_value_t *value, njs_value_t *invld)
{
    njs_release(vm, value);

    njs_set_invalid(value);

    vm->retval = njs_value_true;

    return sizeof(njs_vmcode_2addr_t);
}


njs_ret_t
njs_vmcode_unary_plus(njs_vm_t *vm, njs_value_t *value, njs_value_t *invld)
{
    if (nxt_fast_path(njs_is_numeric(value))) {
        njs_number_set(&vm->retval, value->data.u.number);
        return sizeof(njs_vmcode_2addr_t);
    }

    return NJS_TRAP_NUMBER;
}


njs_ret_t
njs_vmcode_unary_negation(njs_vm_t *vm, njs_value_t *value, njs_value_t *invld)
{
    if (nxt_fast_path(njs_is_numeric(value))) {
        njs_number_set(&vm->retval, - value->data.u.number);
        return sizeof(njs_vmcode_2addr_t);
    }

    return NJS_TRAP_NUMBER;
}


njs_ret_t
njs_vmcode_addition(njs_vm_t *vm, njs_value_t *val1, njs_value_t *val2)
{
    double             num;
    u_char             *start;
    size_t             size, length;
    njs_string_prop_t  string1, string2;

    if (nxt_fast_path(njs_is_numeric(val1) && njs_is_numeric(val2))) {

        num = val1->data.u.number + val2->data.u.number;
        njs_number_set(&vm->retval, num);

        return sizeof(njs_vmcode_3addr_t);
    }

    if (nxt_fast_path(njs_is_string(val1) && njs_is_string(val2))) {

        (void) njs_string_prop(&string1, val1);
        (void) njs_string_prop(&string2, val2);

        if ((string1.length != 0 || string1.size == 0)
            && (string2.length != 0 || string2.size == 0))
        {
            length = string1.length + string2.length;

        } else {
            length = 0;
        }

        size = string1.size + string2.size;

        start = njs_string_alloc(vm, &vm->retval, size, length);

        if (nxt_slow_path(start == NULL)) {
            return NXT_ERROR;
        }

        (void) memcpy(start, string1.start, string1.size);
        (void) memcpy(start + string1.size, string2.start, string2.size);

        if (length >= NJS_STRING_MAP_OFFSET && size != length) {
            njs_string_offset_map_init(start, size);
        }

        return sizeof(njs_vmcode_3addr_t);
    }

    return NJS_TRAP_STRINGS;
}


njs_ret_t
njs_vmcode_substraction(njs_vm_t *vm, njs_value_t *val1, njs_value_t *val2)
{
    double  num;

    if (nxt_fast_path(njs_is_numeric(val1) && njs_is_numeric(val2))) {

        num = val1->data.u.number - val2->data.u.number;
        njs_number_set(&vm->retval, num);

        return sizeof(njs_vmcode_3addr_t);
    }

    return NJS_TRAP_NUMBERS;
}


njs_ret_t
njs_vmcode_multiplication(njs_vm_t *vm, njs_value_t *val1, njs_value_t *val2)
{
    double  num;

    if (nxt_fast_path(njs_is_numeric(val1) && njs_is_numeric(val2))) {

        num = val1->data.u.number * val2->data.u.number;
        njs_number_set(&vm->retval, num);

        return sizeof(njs_vmcode_3addr_t);
    }

    return NJS_TRAP_NUMBERS;
}


njs_ret_t
njs_vmcode_division(njs_vm_t *vm, njs_value_t *val1, njs_value_t *val2)
{
    double  num;

    if (nxt_fast_path(njs_is_numeric(val1) && njs_is_numeric(val2))) {

        num = val1->data.u.number / val2->data.u.number;
        njs_number_set(&vm->retval, num);

        return sizeof(njs_vmcode_3addr_t);
    }

    return NJS_TRAP_NUMBERS;
}


njs_ret_t
njs_vmcode_remainder(njs_vm_t *vm, njs_value_t *val1, njs_value_t *val2)
{
    double  num;

    if (nxt_fast_path(njs_is_numeric(val1) && njs_is_numeric(val2))) {

        num = fmod(val1->data.u.number, val2->data.u.number);
        njs_number_set(&vm->retval, num);

        return sizeof(njs_vmcode_3addr_t);
    }

    return NJS_TRAP_NUMBERS;
}


njs_ret_t
njs_vmcode_left_shift(njs_vm_t *vm, njs_value_t *val1, njs_value_t *val2)
{
    int32_t   num1;
    uint32_t  num2;

    if (nxt_fast_path(njs_is_numeric(val1) && njs_is_numeric(val2))) {

        num1 = njs_integer_value(val1->data.u.number);
        num2 = njs_integer_value(val2->data.u.number);
        njs_number_set(&vm->retval, num1 << (num2 & 0x1f));

        return sizeof(njs_vmcode_3addr_t);
    }

    return NJS_TRAP_NUMBERS;
}


njs_ret_t
njs_vmcode_right_shift(njs_vm_t *vm, njs_value_t *val1, njs_value_t *val2)
{
    int32_t   num1;
    uint32_t  num2;

    if (nxt_fast_path(njs_is_numeric(val1) && njs_is_numeric(val2))) {

        num1 = njs_integer_value(val1->data.u.number);
        num2 = njs_integer_value(val2->data.u.number);
        njs_number_set(&vm->retval, num1 >> (num2 & 0x1f));

        return sizeof(njs_vmcode_3addr_t);
    }

    return NJS_TRAP_NUMBERS;
}


njs_ret_t
njs_vmcode_unsigned_right_shift(njs_vm_t *vm, njs_value_t *val1,
    njs_value_t *val2)
{
    int32_t   num2;
    uint32_t  num1;

    if (nxt_fast_path(njs_is_numeric(val1) && njs_is_numeric(val2))) {

        num1 = njs_integer_value(val1->data.u.number);
        num2 = njs_integer_value(val2->data.u.number);
        njs_number_set(&vm->retval, num1 >> (num2 & 0x1f));

        return sizeof(njs_vmcode_3addr_t);
    }

    return NJS_TRAP_NUMBERS;
}


njs_ret_t
njs_vmcode_logical_not(njs_vm_t *vm, njs_value_t *value, njs_value_t *inlvd)
{
    const njs_value_t  *retval;

    if (njs_is_true(value)) {
        retval = &njs_value_false;

    } else {
        retval = &njs_value_true;
    }

    vm->retval = *retval;

    return sizeof(njs_vmcode_2addr_t);
}


njs_ret_t
njs_vmcode_logical_and(njs_vm_t *vm, njs_value_t *val1, njs_value_t *val2)
{
    njs_value_t  *retval;

    if (njs_is_true(val1)) {
        retval = val2;

    } else {
        retval = val1;
    }

    vm->retval = *retval;

    return sizeof(njs_vmcode_3addr_t);
}


njs_ret_t
njs_vmcode_logical_or(njs_vm_t *vm, njs_value_t *val1, njs_value_t *val2)
{
    njs_value_t  *retval;

    if (njs_is_true(val1)) {
        retval = val1;

    } else {
        retval = val2;
    }

    vm->retval = *retval;

    return sizeof(njs_vmcode_3addr_t);
}


njs_ret_t
njs_vmcode_bitwise_not(njs_vm_t *vm, njs_value_t *value, njs_value_t *invld)
{
    int32_t  num;

    if (nxt_fast_path(njs_is_numeric(value))) {
        num = njs_integer_value(value->data.u.number);
        njs_number_set(&vm->retval, ~num);

        return sizeof(njs_vmcode_2addr_t);
    }

    return NJS_TRAP_NUMBER;
}


njs_ret_t
njs_vmcode_bitwise_and(njs_vm_t *vm, njs_value_t *val1, njs_value_t *val2)
{
    int32_t  num1, num2;

    if (nxt_fast_path(njs_is_numeric(val1) && njs_is_numeric(val2))) {

        num1 = njs_integer_value(val1->data.u.number);
        num2 = njs_integer_value(val2->data.u.number);
        njs_number_set(&vm->retval, num1 & num2);

        return sizeof(njs_vmcode_3addr_t);
    }

    return NJS_TRAP_NUMBERS;
}


njs_ret_t
njs_vmcode_bitwise_xor(njs_vm_t *vm, njs_value_t *val1, njs_value_t *val2)
{
    int32_t  num1, num2;

    if (nxt_fast_path(njs_is_numeric(val1) && njs_is_numeric(val2))) {

        num1 = njs_integer_value(val1->data.u.number);
        num2 = njs_integer_value(val2->data.u.number);
        njs_number_set(&vm->retval, num1 ^ num2);

        return sizeof(njs_vmcode_3addr_t);
    }

    return NJS_TRAP_NUMBERS;
}


njs_ret_t
njs_vmcode_bitwise_or(njs_vm_t *vm, njs_value_t *val1, njs_value_t *val2)
{
    int32_t  num1, num2;

    if (nxt_fast_path(njs_is_numeric(val1) && njs_is_numeric(val2))) {

        num1 = njs_integer_value(val1->data.u.number);
        num2 = njs_integer_value(val2->data.u.number);
        njs_number_set(&vm->retval, num1 | num2);

        return sizeof(njs_vmcode_3addr_t);
    }

    return NJS_TRAP_NUMBERS;
}


static nxt_noinline uint32_t
njs_integer_value(double num)
{
    int64_t  i64;

    /*
     * ECMAScript 5.1: integer must be modulo 2^32.
     * 2^53 is the largest integer number which can be stored in the IEEE-754
     * format and numbers less than 2^53 can be just converted to int64_t
     * eliding more expensive fmod() operation.  Then the int64 integer is
     * truncated to uint32_t.  The NaN can be converted to 0x8000000000000000
     * and becomes 0 after truncation.  fmod() of the infinity returns NaN.
     */

    if (num < 0 || num > 9007199254740992.0) {
        i64 = fmod(num, 4294967296.0);

    } else {
        i64 = num;
    }

    return (uint32_t) i64;
}


njs_ret_t
njs_vmcode_equal(njs_vm_t *vm, njs_value_t *val1, njs_value_t *val2)
{
    njs_ret_t          ret;
    const njs_value_t  *retval;

    ret = njs_values_equal(val1, val2);

    if (nxt_fast_path(ret >= 0)) {

        retval = (ret != 0) ? &njs_value_true : &njs_value_false;
        vm->retval = *retval;

        return sizeof(njs_vmcode_3addr_t);
    }

    return ret;
}


njs_ret_t
njs_vmcode_not_equal(njs_vm_t *vm, njs_value_t *val1, njs_value_t *val2)
{
    njs_ret_t          ret;
    const njs_value_t  *retval;

    ret = njs_values_equal(val1, val2);

    if (nxt_fast_path(ret >= 0)) {

        retval = (ret == 0) ? &njs_value_true : &njs_value_false;
        vm->retval = *retval;

        return sizeof(njs_vmcode_3addr_t);
    }

    return ret;
}


static nxt_noinline njs_ret_t
njs_values_equal(njs_value_t *val1, njs_value_t *val2)
{
    /* Void and null are equal and not comparable with anything else. */
    if (njs_is_null_or_void(val1)) {
        return (njs_is_null_or_void(val2));
    }

    if (njs_is_numeric(val1) && njs_is_numeric(val2)) {
        /* NaNs and Infinities are handled correctly by comparision. */
        return (val1->data.u.number == val2->data.u.number);
    }

    if (val1->type == val2->type) {

        if (njs_is_string(val1)) {
            return njs_string_eq(val1, val2);
        }

        return (val1->data.u.object == val2->data.u.object);
    }

    return NJS_TRAP_NUMBERS;
}


nxt_noinline njs_ret_t
njs_vmcode_less(njs_vm_t *vm, njs_value_t *val1, njs_value_t *val2)
{
    njs_ret_t          ret;
    const njs_value_t  *retval;

    ret = njs_values_compare(val1, val2);

    if (nxt_fast_path(ret >= -1)) {

        retval = (ret > 0) ? &njs_value_true : &njs_value_false;
        vm->retval = *retval;

        return sizeof(njs_vmcode_3addr_t);
    }

    return ret;
}


njs_ret_t
njs_vmcode_greater(njs_vm_t *vm, njs_value_t *val1, njs_value_t *val2)
{
    return njs_vmcode_less(vm, val2, val1);
}


njs_ret_t
njs_vmcode_less_or_equal(njs_vm_t *vm, njs_value_t *val1, njs_value_t *val2)
{
    return njs_vmcode_greater_or_equal(vm, val2, val1);
}


nxt_noinline njs_ret_t
njs_vmcode_greater_or_equal(njs_vm_t *vm, njs_value_t *val1, njs_value_t *val2)
{
    njs_ret_t          ret;
    const njs_value_t  *retval;

    ret = njs_values_compare(val1, val2);

    if (nxt_fast_path(ret >= -1)) {

        retval = (ret == 0) ? &njs_value_true : &njs_value_false;
        vm->retval = *retval;

        return sizeof(njs_vmcode_3addr_t);
    }

    return ret;
}


/*
 * njs_values_compare() returns
 *   1 if val1 is less than val2,
 *   0 if val1 is greater than or equal to val2,
 *  -1 if the values are not comparable,
 *  or negative trap number if convertion to primitive is required.
 */

static nxt_noinline njs_ret_t
njs_values_compare(njs_value_t *val1, njs_value_t *val2)
{
    if (nxt_fast_path(njs_is_numeric(val1) || njs_is_numeric(val2))) {

        if (nxt_fast_path(njs_is_numeric(val1) && njs_is_numeric(val2))) {

            /* NaN and void values are not comparable with anything. */
            if (njs_is_nan(val1->data.u.number)
                || njs_is_nan(val2->data.u.number))
            {
                return -1;
            }

            /* Infinities are handled correctly by comparision. */
            return (val1->data.u.number < val2->data.u.number);
        }

        return NJS_TRAP_NUMBERS;
    }

    if (nxt_fast_path(njs_is_string(val1) && njs_is_string(val2))) {
        return (njs_string_cmp(val1, val2) < 0) ? 1 : 0;
    }

    return NJS_TRAP_STRINGS;
}


njs_ret_t
njs_vmcode_strict_equal(njs_vm_t *vm, njs_value_t *val1, njs_value_t *val2)
{
    const njs_value_t  *retval;

    if (njs_values_strict_equal(val1, val2)) {
        retval = &njs_value_true;

    } else {
        retval = &njs_value_false;
    }

    vm->retval = *retval;

    return sizeof(njs_vmcode_3addr_t);
}


njs_ret_t
njs_vmcode_strict_not_equal(njs_vm_t *vm, njs_value_t *val1, njs_value_t *val2)
{
    const njs_value_t  *retval;

    if (njs_values_strict_equal(val1, val2)) {
        retval = &njs_value_false;

    } else {
        retval = &njs_value_true;
    }

    vm->retval = *retval;

    return sizeof(njs_vmcode_3addr_t);
}


static nxt_noinline nxt_bool_t
njs_values_strict_equal(njs_value_t *val1, njs_value_t *val2)
{
    size_t        size;
    const u_char  *start1, *start2;

    if (val1->type != val2->type) {
        return 0;
    }

    if (njs_is_numeric(val1)) {
        /* NaNs and Infinities are handled correctly by comparision. */
        return (val1->data.u.number == val2->data.u.number);
    }

    if (njs_is_string(val1)) {
        size = val1->short_string.size;

        if (size != val2->short_string.size) {
            return 0;
        }

        if (size != NJS_STRING_LONG) {
            if (val1->short_string.length != val2->short_string.length) {
                return 0;
            }

            start1 = val1->short_string.start;
            start2 = val2->short_string.start;

        } else {
            size = val1->data.string_size;

            if (size != val2->data.string_size) {
                return 0;
            }

            if (val1->data.u.string->length != val2->data.u.string->length) {
                return 0;
            }

            start1 = val1->data.u.string->start;
            start2 = val2->data.u.string->start;
        }

        return (memcmp(start1, start2, size) == 0);
    }

    return (val1->data.u.object == val2->data.u.object);
}


njs_ret_t
njs_vmcode_move(njs_vm_t *vm, njs_value_t *value, njs_value_t *invld)
{
    vm->retval = *value;

    njs_retain(value);

    return sizeof(njs_vmcode_move_t);
}


njs_ret_t
njs_vmcode_validate(njs_vm_t *vm, njs_value_t *invld, njs_value_t *index)
{
    njs_value_t  *value;

    value = njs_vmcode_operand(vm, index);

    if (nxt_fast_path(njs_is_valid(value))) {
        return sizeof(njs_vmcode_validate_t);
    }

    vm->exception = &njs_exception_reference_error;

    return NXT_ERROR;
}


njs_ret_t
njs_vmcode_jump(njs_vm_t *vm, njs_value_t *invld, njs_value_t *offset)
{
    return (njs_ret_t) offset;
}


njs_ret_t
njs_vmcode_if_true_jump(njs_vm_t *vm, njs_value_t *cond, njs_value_t *offset)
{
    if (njs_is_true(cond)) {
        return (njs_ret_t) offset;
    }

    return sizeof(njs_vmcode_cond_jump_t);
}


njs_ret_t
njs_vmcode_if_false_jump(njs_vm_t *vm, njs_value_t *cond, njs_value_t *offset)
{
    if (njs_is_true(cond)) {
        return sizeof(njs_vmcode_cond_jump_t);
    }

    return (njs_ret_t) offset;
}


njs_ret_t
njs_vmcode_function_frame(njs_vm_t *vm, njs_value_t *invld, njs_value_t *name)
{
    njs_ret_t                    ret;
    njs_value_t                  val, *this, *value;
    njs_param_t                  param;
    njs_object_t                 *object;
    njs_function_t               *function;
    njs_vmcode_function_frame_t  *func;

    value = njs_vmcode_operand(vm, name);

    if (nxt_fast_path(njs_is_function(value))) {

        func = (njs_vmcode_function_frame_t *) vm->current;

        function = value->data.u.function;

        if (function->native) {
            this = njs_function_native_frame(vm, function->u.native, 0,
                                             &func->code);
            if (nxt_fast_path(this != NULL)) {
                *this = njs_value_void;

                return sizeof(njs_vmcode_function_frame_t);
            }

            return NXT_ERROR;
        }

        if (func->code.ctor) {
            object = njs_object_alloc(vm);

            if (nxt_slow_path(object == NULL)) {
                return NXT_ERROR;
            }

            val.data.u.object = object;
            val.type = NJS_OBJECT;
            val.data.truth = 1;
            param.object = &val;

        } else {
            param.object = (njs_value_t *) &njs_value_void;
        }

        param.args = NULL;
        param.nargs = func->code.nargs;

        ret = njs_function_frame(vm, function, &param, func->code.ctor);

        if (nxt_fast_path(ret == NXT_OK)) {
            return sizeof(njs_vmcode_function_frame_t);
        }

        return ret;
    }

    vm->exception = &njs_exception_type_error;

    return NXT_ERROR;
}


njs_ret_t
njs_vmcode_method_frame(njs_vm_t *vm, njs_value_t *name, njs_value_t *object)
{
    njs_ret_t                  ret;
    njs_value_t                *this;
    njs_param_t                param;
    njs_extern_t               *ext;
    njs_function_t             *function;
    njs_object_prop_t          *prop;
    njs_property_query_t       pq;
    njs_vmcode_method_frame_t  *method;

    object = njs_vmcode_operand(vm, object);

    pq.query = NJS_PROPERTY_QUERY_GET;

    switch (njs_property_query(vm, &pq, object, name)) {

    case NXT_OK:
        prop = pq.lhq.value;

        if (njs_is_function(&prop->value)) {

            method = (njs_vmcode_method_frame_t *) vm->current;
            function = prop->value.data.u.function;

            if (!function->native) {
                param.object = object;
                param.args = NULL;
                param.nargs = method->code.nargs;

                ret = njs_function_frame(vm, function, &param,
                                         method->code.ctor);

                if (nxt_fast_path(ret == NXT_OK)) {
                    return sizeof(njs_vmcode_method_frame_t);
                }

                return ret;
            }

            this = njs_function_native_frame(vm, function->u.native,
                                             prop->value.data.string_size,
                                             &method->code);
            if (nxt_slow_path(this == NULL)) {
                return NXT_ERROR;
            }

            njs_retain(object);
            *this = *object;

            return sizeof(njs_vmcode_method_frame_t);
        }

        break;

    case NJS_EXTERNAL_VALUE:
        ext = object->data.u.external;

        ret = nxt_lvlhsh_find(&ext->hash, &pq.lhq);

        if (ret == NXT_OK) {
            method = (njs_vmcode_method_frame_t *) vm->current;
            ext = pq.lhq.value;

            if (ext->type == NJS_EXTERN_METHOD) {
                this = njs_function_native_frame(vm, ext->method, 0,
                                                 &method->code);

                if (nxt_slow_path(this == NULL)) {
                    return NXT_ERROR;
                }

                this->data.u.data = vm->external[ext->object];

                return sizeof(njs_vmcode_method_frame_t);
            }
        }

        break;

    default:
        break;
    }

    vm->exception = &njs_exception_type_error;

    return NXT_ERROR;
}


njs_ret_t
njs_vmcode_function_call(njs_vm_t *vm, njs_value_t *invld, njs_value_t *retval)
{
    njs_ret_t                   ret;
    njs_value_t                 *args;
    njs_param_t                 param;
    njs_native_frame_t          *frame, *previous, *skip;
    njs_vmcode_function_call_t  *call;

    call = (njs_vmcode_function_call_t *) vm->current;
    vm->current += sizeof(njs_vmcode_function_call_t);

    if (!vm->frame->native) {
        (void) njs_function_call(vm, (njs_index_t) retval);
        return 0;
    }

    param.retval = (njs_index_t) retval;
    param.nargs = call->code.nargs - 1;
    args = vm->scopes[NJS_SCOPE_CALLEE_ARGUMENTS];
    param.args = args;
    param.object = args - 1;

    ret = vm->frame->u.native(vm, &param);
    /*
     * A native method can return:
     *    NXT_OK on method success;
     *    NJS_PASS by Function.apply() and Function.call();
     *    NXT_AGAIN to postpone nJSVM processing;
     *    NXT_ERROR.
     *
     * The callee arguments must be preserved for NJS_PASS and NXT_AGAIN cases.
     */
    if (ret == NXT_OK) {
        skip = NULL;
        frame = vm->frame;
        previous = frame->previous;

        if (previous->skip) {

            if (previous->first) {
                skip = previous;
            }

            previous = previous->previous;
        }

        vm->frame = previous;

        (void) njs_function_frame_free(vm, frame, skip);

        /*
         * If a retval is in a callee arguments scope it
         * must be in the previous callee arguments scope.
         */
        vm->scopes[NJS_SCOPE_CALLEE_ARGUMENTS] = vm->frame->arguments;

        retval = njs_vmcode_operand(vm, retval);
         /*
          * GC: value external/internal++ depending
          * on vm->retval and retval type
          */
        *retval = vm->retval;

    } else if (ret == NJS_PASS) {
        ret = 0;

    } else if (ret == NXT_AGAIN) {
        vm->frame->reentrant = 1;
    }

    return ret;
}


njs_ret_t
njs_vmcode_return(njs_vm_t *vm, njs_value_t *invld, njs_value_t *retval)
{
    njs_value_t         *value;
    njs_frame_t         *frame;
    njs_value_t         *args;
    njs_native_frame_t  *previous, *skip;

    value = njs_vmcode_operand(vm, retval);

    frame = (njs_frame_t *) vm->frame;

    if (frame->native.ctor) {
        if (njs_is_object(value)) {
            njs_release(vm, vm->scopes[NJS_SCOPE_ARGUMENTS]);

        } else {
            value = vm->scopes[NJS_SCOPE_ARGUMENTS];
        }
    }

    skip = NULL;
    previous = frame->native.previous;

    if (previous->skip) {

        if (previous->first) {
            skip = previous;
        }

        previous = previous->previous;
    }

    vm->frame = previous;

    vm->scopes[NJS_SCOPE_CALLEE_ARGUMENTS] = previous->arguments;
    vm->scopes[NJS_SCOPE_LOCAL] = frame->prev_local;
    args = vm->scopes[NJS_SCOPE_ARGUMENTS];
    vm->scopes[NJS_SCOPE_ARGUMENTS] = frame->prev_arguments;

    /*
     * If a retval is in a callee arguments scope it
     * must be in the previous callee arguments scope.
     */
    retval = njs_vmcode_operand(vm, frame->retval);

    /* GC: value external/internal++ depending on value and retval type */
    *retval = *value;

    vm->current = frame->native.u.return_address;

    /* GC: arguments and local. */

    njs_release(vm, &args[0]);

    return njs_function_frame_free(vm, &frame->native, skip);
}


static nxt_noinline njs_ret_t
njs_function_frame_free(njs_vm_t *vm, njs_native_frame_t *frame,
    njs_native_frame_t *skip)
{
    if (frame->first) {
        nxt_mem_cache_free(vm->mem_cache_pool, frame);
    }

    if (skip != NULL) {
        nxt_mem_cache_free(vm->mem_cache_pool, skip);
    }

    return 0;
}


njs_ret_t
njs_vmcode_stop(njs_vm_t *vm, njs_value_t *invld, njs_value_t *retval)
{
    njs_value_t  *value;

    value = njs_vmcode_operand(vm, retval);

    vm->retval = *value;

    return NXT_DONE;
}


/*
 * njs_vmcode_try_start() is set on the start of a "try" block to create
 * a "try" block, to set a catch address to the start of a "catch" or
 * "finally" blocks and to initialize a value to track uncaught exception.
 */

njs_ret_t
njs_vmcode_try_start(njs_vm_t *vm, njs_value_t *value, njs_value_t *offset)
{
    njs_exception_t  *e;

    if (vm->frame->exception.catch != NULL) {
        e = nxt_mem_cache_alloc(vm->mem_cache_pool, sizeof(njs_exception_t));
        if (nxt_slow_path(e == NULL)) {
            return NXT_ERROR;
        }

        *e = vm->frame->exception;
        vm->frame->exception.next = e;
    }

    vm->frame->exception.catch = vm->current + (njs_ret_t) offset;

    njs_set_invalid(value);

    return sizeof(njs_vmcode_try_start_t);
}


/*
 * njs_vmcode_try_end() is set on the end of a "try" block to remove the block.
 * It is also set on the end of a "catch" block followed by a "finally" block.
 */

nxt_noinline njs_ret_t
njs_vmcode_try_end(njs_vm_t *vm, njs_value_t *invld, njs_value_t *offset)
{
    njs_exception_t  *e;

    e = vm->frame->exception.next;

    if (e == NULL) {
        vm->frame->exception.catch = NULL;

    } else {
        vm->frame->exception = *e;
        nxt_mem_cache_free(vm->mem_cache_pool, e);
    }

    return (njs_ret_t) offset;
}


njs_ret_t
njs_vmcode_throw(njs_vm_t *vm, njs_value_t *invld, njs_value_t *retval)
{
    njs_value_t  *value;

    value = njs_vmcode_operand(vm, retval);

    vm->retval = *value;

    return NXT_ERROR;
}


/*
 * njs_vmcode_catch() is set on the start of a "catch" block to store
 * exception and to remove a "try" block if there is no "finally" block
 * or to update a catch address to the start of a "finally" block.
 * njs_vmcode_catch() is set on the start of a "finally" block to store
 * uncaught exception and to remove a "try" block.
 */

njs_ret_t
njs_vmcode_catch(njs_vm_t *vm, njs_value_t *exception, njs_value_t *offset)
{
    *exception = vm->retval;

    if ((njs_ret_t) offset == sizeof(njs_vmcode_catch_t)) {
        return njs_vmcode_try_end(vm, exception, offset);
    }

    vm->frame->exception.catch = vm->current + (njs_ret_t) offset;

    return sizeof(njs_vmcode_catch_t);
}


/*
 * njs_vmcode_finally() is set on the end of a "finally" block to throw
 * uncaught exception.
 */

njs_ret_t
njs_vmcode_finally(njs_vm_t *vm, njs_value_t *invld, njs_value_t *retval)
{
    njs_value_t  *value;

    value = njs_vmcode_operand(vm, retval);

    if (!njs_is_valid(value)) {
        return sizeof(njs_vmcode_finally_t);
    }

    vm->retval = *value;

    return NXT_ERROR;
}


njs_ret_t
njs_vmcode_number_primitive(njs_vm_t *vm, njs_value_t *invld, njs_value_t *narg)
{
    double       num;
    njs_ret_t    ret;
    njs_value_t  *value;

    value = njs_native_data(vm->frame);
    value = &value[(uintptr_t) narg + 1];

    ret = njs_primitive_value(vm, value, 0);

    if (nxt_fast_path(ret > 0)) {

        if (!njs_is_numeric(value)) {
            num = NJS_NAN;

            if (njs_is_string(value)) {
                num = njs_string_to_number(value);
            }

            njs_number_set(value, num);
        }

        ret = sizeof(njs_vmcode_1addr_t);
    }

    return ret;
}


njs_ret_t
njs_vmcode_string_primitive(njs_vm_t *vm, njs_value_t *invld, njs_value_t *narg)
{
    njs_ret_t          ret;
    njs_value_t        *value;
    const njs_value_t  *string;

    value = njs_native_data(vm->frame);
    value = &value[(uintptr_t) narg + 1];

    ret = njs_primitive_value(vm, value, 1);

    if (nxt_fast_path(ret > 0)) {

        if (!njs_is_string(value)) {

            switch (value->type) {

            case NJS_NULL:
                string = &njs_string_null;
                break;

            case NJS_VOID:
                string = &njs_string_void;
                break;

            case NJS_BOOLEAN:
                string = njs_is_true(value) ? &njs_string_true:
                                              &njs_string_false;
                break;

            case NJS_NUMBER:
                ret = njs_number_to_string(vm, value, value);
                if (nxt_fast_path(ret == NXT_OK)) {
                    goto done;
                }

            default:
                return NXT_ERROR;
            }

            *value = *string;
        }

    done:

        ret = sizeof(njs_vmcode_1addr_t);
    }

    return ret;
}


/*
 * A hint value is 0 for numbers and 1 for strings.  The value chooses
 * method calls order specified by ECMAScript 5.1: "valueOf", "toString"
 * for numbers and "toString", "valueOf" for strings.
 */

static nxt_noinline njs_ret_t
njs_primitive_value(njs_vm_t *vm, njs_value_t *value, nxt_uint_t hint)
{
    njs_ret_t           ret;
    njs_param_t         param;
    njs_value_t         *retval;
    njs_object_prop_t   *prop;
    nxt_lvlhsh_query_t  lhq;

    static const uint32_t  hashes[] = {
        NJS_VALUE_OF_HASH,
        NJS_TO_STRING_HASH,
    };

    static const nxt_str_t  names[] = {
        nxt_string("valueOf"),
        nxt_string("toString"),
    };

    if (!njs_is_primitive(value)) {
        retval = njs_native_data(vm->frame);

        if (!njs_is_valid(retval)) {

            for ( ;; ) {
                vm->exception = &njs_exception_type_error;
                ret = NXT_ERROR;

                if (njs_is_object(value) && vm->frame->reentrant < 2) {
                    hint ^= vm->frame->reentrant++;

                    lhq.key_hash = hashes[hint];
                    lhq.key = names[hint];

                    prop = njs_object_property(vm, value->data.u.object, &lhq);

                    if (nxt_fast_path(prop != NULL)) {
                        param.object = value;
                        param.retval = (njs_index_t) retval;
                        param.args = NULL;
                        param.nargs = 0;

                        ret = njs_function_apply(vm, &prop->value, &param);

                        /*
                         * njs_function_apply() can return
                         *     NXT_OK, NJS_PASS, NXT_ERROR, NXT_AGAIN.
                         */
                        if (nxt_fast_path(ret == NXT_OK)) {

                            if (njs_is_primitive(&vm->retval)) {
                                retval = &vm->retval;
                                break;
                            }

                            continue;
                        }

                        if (ret == NJS_PASS) {
                            ret = 0;
                        }
                    }
                }

                return ret;
            }
        }

        *value = *retval;

        njs_set_invalid(retval);
    }

    vm->frame->reentrant = 0;

    return 1;
}


static const njs_vmcode_1addr_t  njs_trap_strings[] = {
    { .code = { .operation = njs_vmcode_string_primitive,
                .operands =  NJS_VMCODE_1OPERAND,
                .retval = NJS_VMCODE_NO_RETVAL },
      .index = 0 },
    { .code = { .operation = njs_vmcode_string_primitive,
                .operands =  NJS_VMCODE_1OPERAND,
                .retval = NJS_VMCODE_NO_RETVAL },
      .index = 1 },
    { .code = { .operation = njs_vmcode_restart,
                .operands =  NJS_VMCODE_NO_OPERAND,
                .retval = NJS_VMCODE_NO_RETVAL } },
};


static const njs_vmcode_1addr_t  njs_trap_numbers[] = {
    { .code = { .operation = njs_vmcode_number_primitive,
                .operands =  NJS_VMCODE_1OPERAND,
                .retval = NJS_VMCODE_NO_RETVAL },
      .index = 0 },
    { .code = { .operation = njs_vmcode_number_primitive,
                .operands =  NJS_VMCODE_1OPERAND,
                .retval = NJS_VMCODE_NO_RETVAL },
      .index = 1 },
    { .code = { .operation = njs_vmcode_restart,
                .operands =  NJS_VMCODE_NO_OPERAND,
                .retval = NJS_VMCODE_NO_RETVAL } },
};


static const njs_vmcode_1addr_t  njs_trap_number[] = {
    { .code = { .operation = njs_vmcode_number_primitive,
                .operands =  NJS_VMCODE_1OPERAND,
                .retval = NJS_VMCODE_NO_RETVAL },
      .index = 0 },
    { .code = { .operation = njs_vmcode_restart,
                .operands =  NJS_VMCODE_NO_OPERAND,
                .retval = NJS_VMCODE_NO_RETVAL } },
};


static const njs_vm_trap_t  njs_vm_traps[] = {
    /* NJS_TRAP_PROPERTY */  { &njs_trap_strings[1], 0 },
    /* NJS_TRAP_STRINGS */   { &njs_trap_strings[0], 0 },
    /* NJS_TRAP_INCDEC */    { &njs_trap_numbers[1], 1 },
    /* NJS_TRAP_NUMBERS */   { &njs_trap_numbers[0], 0 },
    /* NJS_TRAP_NUMBER */    { &njs_trap_number[0],  0 },
};


static njs_ret_t
njs_vm_trap(njs_vm_t *vm, nxt_uint_t trap, njs_value_t *value1,
    njs_value_t *value2)
{
    size_t              size;
    njs_value_t         *values;
    njs_native_frame_t  *frame;

    size = NJS_NATIVE_FRAME_SIZE + 3 * sizeof(njs_value_t);

    frame = njs_function_frame_alloc(vm, size);
    if (nxt_slow_path(frame == NULL)) {
        return NXT_ERROR;
    }

    frame->ctor = 0;

    values = njs_native_data(frame);
    njs_set_invalid(&values[0]);
    values[2] = *value2;

    frame->trap_reference = njs_vm_traps[trap].reference_value;

    if (njs_vm_traps[trap].reference_value) {
        values[1].data.u.value = value1;

    } else {
        values[1] = *value1;
    }

    frame->u.restart = vm->current;
    vm->current = (u_char *) njs_vm_traps[trap].code;

    return NXT_OK;
}


static njs_ret_t
njs_vmcode_restart(njs_vm_t *vm, njs_value_t *invld1, njs_value_t *invld2)
{
    u_char                *restart;
    njs_ret_t             ret;
    njs_value_t           *retval, *values, *value1;
    njs_native_frame_t    *frame;
    njs_vmcode_generic_t  *vmcode;

    frame = vm->frame;
    vm->frame = frame->previous;
    vm->scopes[NJS_SCOPE_CALLEE_ARGUMENTS] = frame->previous->arguments;

    restart = frame->u.restart;
    vm->current = restart;
    vmcode = (njs_vmcode_generic_t *) restart;

    values = njs_native_data(frame);
    value1 = &values[1];

    if (frame->trap_reference) {
        value1 = value1->data.u.value;
    }

    ret = vmcode->code.operation(vm, value1, &values[2]);

    retval = njs_vmcode_operand(vm, vmcode->operand1);

    //njs_release(vm, retval);

    *retval = vm->retval;

    if (frame->first) {
        nxt_mem_cache_free(vm->mem_cache_pool, frame);
    }

    return ret;
}


nxt_noinline void
njs_number_set(njs_value_t *value, double num)
{
    value->data.u.number = num;
    value->type = NJS_NUMBER;
    value->data.truth = njs_is_number_true(num);
}


njs_ret_t
njs_void_set(njs_value_t *value)
{
    *value = njs_value_void;
    return NXT_OK;
}


void *
njs_value_data(njs_value_t *value)
{
    return value->data.u.data;
}


nxt_uint_t
njs_vm_is_reentrant(njs_vm_t *vm)
{
    return vm->frame->reentrant;
}


nxt_int_t
njs_value_string(njs_vm_t *vm, nxt_str_t *retval, njs_value_t *value,
    njs_value_t **tmp)
{
    size_t       size;
    nxt_int_t    ret;
    njs_value_t  *val;

    val = value;
    *tmp = NULL;

    if (!njs_is_string(val)) {
        val = nxt_mem_cache_alloc(vm->mem_cache_pool, sizeof(njs_value_t));
        if (nxt_slow_path(val == NULL)) {
            return NXT_ERROR;
        }

        ret = njs_value_to_string(vm, val, value);
        if (nxt_slow_path(ret != NXT_OK)) {
            /* TODO: njs_free(vm, val); */
            return ret;
        }

        *tmp = val;
    }

    size = val->short_string.size;

    if (size != NJS_STRING_LONG) {
        retval->len = size;
        retval->data = val->short_string.start;

    } else {
        njs_retain(val);
        retval->len = val->data.string_size;
        retval->data = val->data.u.string->start;
    }

    return NXT_OK;
}


nxt_int_t
njs_value_string_copy(njs_vm_t *vm, nxt_str_t *retval, njs_value_t *value,
    uintptr_t *next)
{
    uintptr_t    n;
    njs_array_t  *array;

    switch (value->type) {

    case NJS_STRING:
        if (*next != 0) {
            return NXT_DECLINED;
        }

        *next = 1;
        break;

    case NJS_ARRAY:
        array = value->data.u.array;

        do {
            n = (*next)++;

            if (n == array->length) {
                return NXT_DECLINED;
            }

            value = &array->start[n];

        } while (!njs_is_valid(value));

        break;

    default:
        return NXT_ERROR;
    }

    return njs_value_to_ext_string(vm, retval, value);
}


void
njs_debug(njs_index_t index, njs_value_t *value)
{
#if (NXT_DEBUG)
    u_char    *p;
    uint32_t  len;

    switch (value->type) {

    case NJS_NULL:
        nxt_thread_log_debug("%p [null]", index);
        return;

    case NJS_VOID:
        nxt_thread_log_debug("%p [void]", index);
        return;

    case NJS_BOOLEAN:
        nxt_thread_log_debug("%p [%s]", index,
                             (value->data.u.number == 0.0) ? "false" : "true");
        return;

    case NJS_NUMBER:
        nxt_thread_log_debug("%p [%f]", index, value->data.u.number);
        return;

    case NJS_STRING:
        len = value->short_string.size;
        if (len != NJS_STRING_LONG) {
            p = value->short_string.start;

        } else {
            len = value->data.string_size;
            p = value->data.u.string->start;
        }

        nxt_thread_log_debug("%p [\"%*s\"]", index, len, p);
        return;

    case NJS_ARRAY:
        nxt_thread_log_debug("%p [array]", index);
        return;

    default:
        nxt_thread_log_debug("%p [invalid]", index);
        return;
    }
#endif
}


void *
njs_lvlhsh_alloc(void *data, size_t size, nxt_uint_t nalloc)
{
    return nxt_mem_cache_align(data, size, size);
}


void
njs_lvlhsh_free(void *data, void *p, size_t size)
{
    nxt_mem_cache_free(data, p);
}
