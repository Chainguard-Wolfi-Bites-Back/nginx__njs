
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) NGINX, Inc.
 */


#include <njs_main.h>


struct njs_regexp_group_s {
    njs_str_t  name;
    uint32_t   hash;
    uint32_t   capture;
};


static void *njs_regexp_malloc(size_t size, void *memory_data);
static void njs_regexp_free(void *p, void *memory_data);
static njs_int_t njs_regexp_prototype_source(njs_vm_t *vm, njs_value_t *args,
    njs_uint_t nargs, njs_index_t unused);
static int njs_regexp_pattern_compile(njs_vm_t *vm, njs_regex_t *regex,
    u_char *source, int options);
static u_char *njs_regexp_compile_trace_handler(njs_trace_t *trace,
    njs_trace_data_t *td, u_char *start);
static u_char *njs_regexp_match_trace_handler(njs_trace_t *trace,
    njs_trace_data_t *td, u_char *start);
static njs_array_t *njs_regexp_exec_result(njs_vm_t *vm, njs_regexp_t *regexp,
    njs_regexp_utf8_t type, njs_string_prop_t *string,
    njs_regex_match_data_t *data);
static njs_int_t njs_regexp_string_create(njs_vm_t *vm, njs_value_t *value,
    u_char *start, uint32_t size, int32_t length);


njs_int_t
njs_regexp_init(njs_vm_t *vm)
{
    vm->regex_context = njs_regex_context_create(njs_regexp_malloc,
                                          njs_regexp_free, vm->mem_pool);
    if (njs_slow_path(vm->regex_context == NULL)) {
        njs_memory_error(vm);
        return NJS_ERROR;
    }

    vm->single_match_data = njs_regex_match_data(NULL, vm->regex_context);
    if (njs_slow_path(vm->single_match_data == NULL)) {
        njs_memory_error(vm);
        return NJS_ERROR;
    }

    vm->regex_context->trace = &vm->trace;

    return NJS_OK;
}


static void *
njs_regexp_malloc(size_t size, void *memory_data)
{
    return njs_mp_alloc(memory_data, size);
}


static void
njs_regexp_free(void *p, void *memory_data)
{
    njs_mp_free(memory_data, p);
}


static njs_regexp_flags_t
njs_regexp_value_flags(njs_vm_t *vm, const njs_value_t *regexp)
{
    njs_regexp_flags_t    flags;
    njs_regexp_pattern_t  *pattern;

    flags = 0;

    pattern = njs_regexp_pattern(regexp);

    if (pattern->global) {
        flags |= NJS_REGEXP_GLOBAL;
    }

    if (pattern->ignore_case) {
        flags |= NJS_REGEXP_IGNORE_CASE;
    }

    if (pattern->multiline) {
        flags |= NJS_REGEXP_MULTILINE;
    }

    return flags;
}


static njs_int_t
njs_regexp_constructor(njs_vm_t *vm, njs_value_t *args, njs_uint_t nargs,
    njs_index_t unused)
{
    u_char              *start;
    njs_int_t           ret;
    njs_str_t           string;
    njs_value_t         source, *pattern, *flags;
    njs_regexp_flags_t  re_flags;

    pattern = njs_arg(args, nargs, 1);

    if (njs_is_regexp(pattern)) {
        ret = njs_regexp_prototype_source(vm, pattern, 1, 0);
        if (njs_slow_path(ret != NJS_OK)) {
            return ret;
        }

        source = vm->retval;

        re_flags = njs_regexp_value_flags(vm, pattern);

        pattern = &source;

    } else {
        if (njs_is_defined(pattern)) {
            ret = njs_value_to_string(vm, pattern, pattern);
            if (njs_slow_path(ret != NJS_OK)) {
                return ret;
            }

        } else {
            pattern = njs_value_arg(&njs_string_empty);
        }

        re_flags = 0;
    }

    flags = njs_arg(args, nargs, 2);

    if (njs_is_defined(flags)) {
        ret = njs_value_to_string(vm, flags, flags);
        if (njs_slow_path(ret != NJS_OK)) {
            return ret;
        }

        njs_string_get(flags, &string);

        start = string.start;

        re_flags = njs_regexp_flags(&start, start + string.length);
        if (njs_slow_path(re_flags < 0
                          || (size_t) (start - string.start) != string.length))
        {
            njs_syntax_error(vm, "Invalid RegExp flags \"%V\"", &string);
            return NJS_ERROR;
        }
    }

    njs_string_get(pattern, &string);

    return njs_regexp_create(vm, &vm->retval, string.start, string.length,
                             re_flags);
}


njs_int_t
njs_regexp_create(njs_vm_t *vm, njs_value_t *value, u_char *start,
    size_t length, njs_regexp_flags_t flags)
{
    njs_regexp_t          *regexp;
    njs_regexp_pattern_t  *pattern;

    if (length != 0 || flags != 0) {
        if (length == 0) {
            start = (u_char *) "(?:)";
            length = njs_length("(?:)");
        }

        pattern = njs_regexp_pattern_create(vm, start, length, flags);
        if (njs_slow_path(pattern == NULL)) {
            return NJS_ERROR;
        }

    } else {
        pattern = vm->shared->empty_regexp_pattern;
    }

    regexp = njs_regexp_alloc(vm, pattern);

    if (njs_fast_path(regexp != NULL)) {
        njs_set_regexp(value, regexp);

        return NJS_OK;
    }

    return NJS_ERROR;
}


/*
 * 1) PCRE with PCRE_JAVASCRIPT_COMPAT flag rejects regexps with
 * lone closing square brackets as invalid.  Whereas according
 * to ES6: 11.8.5 it is a valid regexp expression.
 *
 * 2) escaping zero byte characters as "\u0000".
 *
 * Escaping it here as a workaround.
 */

njs_inline njs_int_t
njs_regexp_escape(njs_vm_t *vm, njs_str_t *text)
{
    size_t      brackets, zeros;
    u_char      *p, *dst, *start, *end;
    njs_bool_t  in;

    start = text->start;
    end = text->start + text->length;

    in = 0;
    zeros = 0;
    brackets = 0;

    for (p = start; p < end; p++) {

        switch (*p) {
        case '[':
            in = 1;
            break;

        case ']':
            if (!in) {
                brackets++;
            }

            in = 0;
            break;

        case '\\':
            p++;

            if (p == end || *p != '\0') {
                break;
            }

            /* Fall through. */

        case '\0':
            zeros++;
            break;
        }
    }

    if (!brackets && !zeros) {
        return NJS_OK;
    }

    text->length = text->length + brackets + zeros * njs_length("\\u0000");

    text->start = njs_mp_alloc(vm->mem_pool, text->length);
    if (njs_slow_path(text->start == NULL)) {
        njs_memory_error(vm);
        return NJS_ERROR;
    }

    in = 0;
    dst = text->start;

    for (p = start; p < end; p++) {

        switch (*p) {
        case '[':
            in = 1;
            break;

        case ']':
            if (!in) {
                *dst++ = '\\';
            }

            in = 0;
            break;

        case '\\':
            *dst++ = *p++;

            if (p == end) {
                goto done;
            }

            if (*p != '\0') {
                break;
            }

            /* Fall through. */

        case '\0':
            dst = njs_cpymem(dst, "\\u0000", 6);
            continue;
        }

        *dst++ = *p;
    }

done:

    text->length = dst - text->start;

    return NJS_OK;
}


njs_regexp_flags_t
njs_regexp_flags(u_char **start, u_char *end)
{
    u_char              *p;
    njs_regexp_flags_t  flags, flag;

    flags = NJS_REGEXP_NO_FLAGS;

    for (p = *start; p < end; p++) {
        switch (*p) {
        case 'g':
            flag = NJS_REGEXP_GLOBAL;
            break;

        case 'i':
            flag = NJS_REGEXP_IGNORE_CASE;
            break;

        case 'm':
            flag = NJS_REGEXP_MULTILINE;
            break;

        default:
            if (*p >= 'a' && *p <= 'z') {
                goto invalid;
            }

            goto done;
        }

        if (njs_slow_path((flags & flag) != 0)) {
            goto invalid;
        }

        flags |= flag;
    }

done:

    *start = p;

    return flags;

invalid:

    *start = p + 1;

    return NJS_REGEXP_INVALID_FLAG;
}


njs_regexp_pattern_t *
njs_regexp_pattern_create(njs_vm_t *vm, u_char *start, size_t length,
    njs_regexp_flags_t flags)
{
    int                   options, ret;
    u_char                *p, *end;
    size_t                size;
    njs_str_t             text;
    njs_uint_t            n;
    njs_regex_t           *regex;
    njs_regexp_group_t    *group;
    njs_regexp_pattern_t  *pattern;

    size = 1;  /* A trailing "/". */
    size += ((flags & NJS_REGEXP_GLOBAL) != 0);
    size += ((flags & NJS_REGEXP_IGNORE_CASE) != 0);
    size += ((flags & NJS_REGEXP_MULTILINE) != 0);

    text.start = start;
    text.length = length;

    ret = njs_regexp_escape(vm, &text);
    if (njs_slow_path(ret != NJS_OK)) {
        return NULL;
    }

    pattern = njs_mp_zalloc(vm->mem_pool, sizeof(njs_regexp_pattern_t) + 1
                                          + text.length + size + 1);
    if (njs_slow_path(pattern == NULL)) {
        njs_memory_error(vm);
        return NULL;
    }

    pattern->flags = size;

    p = (u_char *) pattern + sizeof(njs_regexp_pattern_t);
    pattern->source = p;

    *p++ = '/';
    p = memcpy(p, text.start, text.length);
    p += text.length;
    end = p;
    *p++ = '\0';

    pattern->global = ((flags & NJS_REGEXP_GLOBAL) != 0);
    if (pattern->global) {
        *p++ = 'g';
    }

#ifdef PCRE_JAVASCRIPT_COMPAT
    /* JavaScript compatibility has been introduced in PCRE-7.7. */
    options = PCRE_JAVASCRIPT_COMPAT;
#else
    options = 0;
#endif

    pattern->ignore_case = ((flags & NJS_REGEXP_IGNORE_CASE) != 0);
    if (pattern->ignore_case) {
        *p++ = 'i';
         options |= PCRE_CASELESS;
    }

    pattern->multiline = ((flags & NJS_REGEXP_MULTILINE) != 0);
    if (pattern->multiline) {
        *p++ = 'm';
         options |= PCRE_MULTILINE;
    }

    *p++ = '\0';

    ret = njs_regexp_pattern_compile(vm, &pattern->regex[0],
                                     &pattern->source[1], options);

    if (njs_fast_path(ret >= 0)) {
        pattern->ncaptures = ret;

    } else if (ret < 0 && ret != NJS_DECLINED) {
        goto fail;
    }

    ret = njs_regexp_pattern_compile(vm, &pattern->regex[1],
                                     &pattern->source[1], options | PCRE_UTF8);
    if (njs_fast_path(ret >= 0)) {

        if (njs_slow_path(njs_regex_is_valid(&pattern->regex[0])
                          && (u_int) ret != pattern->ncaptures))
        {
            njs_internal_error(vm, "regexp pattern compile failed");
            goto fail;
        }

        pattern->ncaptures = ret;

    } else if (ret != NJS_DECLINED) {
        goto fail;
    }

    if (njs_regex_is_valid(&pattern->regex[0])) {
        regex = &pattern->regex[0];

    } else if (njs_regex_is_valid(&pattern->regex[1])) {
        regex = &pattern->regex[1];

    } else {
        goto fail;
    }

    *end = '/';

    pattern->ngroups = njs_regex_named_captures(regex, NULL, 0);

    if (pattern->ngroups != 0) {
        size = sizeof(njs_regexp_group_t) * pattern->ngroups;

        pattern->groups = njs_mp_alloc(vm->mem_pool, size);
        if (njs_slow_path(pattern->groups == NULL)) {
            njs_memory_error(vm);
            return NULL;
        }

        n = 0;

        do {
            group = &pattern->groups[n];

            group->capture = njs_regex_named_captures(regex, &group->name, n);
            group->hash = njs_djb_hash(group->name.start, group->name.length);

            n++;

        } while (n != pattern->ngroups);
    }

    njs_set_undefined(&vm->retval);

    return pattern;

fail:

    njs_mp_free(vm->mem_pool, pattern);
    return NULL;
}


static int
njs_regexp_pattern_compile(njs_vm_t *vm, njs_regex_t *regex, u_char *source,
    int options)
{
    njs_int_t            ret;
    njs_trace_handler_t  handler;

    handler = vm->trace.handler;
    vm->trace.handler = njs_regexp_compile_trace_handler;

    /* Zero length means a zero-terminated string. */
    ret = njs_regex_compile(regex, source, 0, options, vm->regex_context);

    vm->trace.handler = handler;

    if (njs_fast_path(ret == NJS_OK)) {
        return regex->ncaptures;
    }

    return ret;
}


static u_char *
njs_regexp_compile_trace_handler(njs_trace_t *trace, njs_trace_data_t *td,
    u_char *start)
{
    u_char    *p;
    njs_vm_t  *vm;

    vm = trace->data;

    trace = trace->next;
    p = trace->handler(trace, td, start);

    njs_syntax_error(vm, "%*s", p - start, start);

    return p;
}


njs_int_t
njs_regexp_match(njs_vm_t *vm, njs_regex_t *regex, const u_char *subject,
    size_t off, size_t len, njs_regex_match_data_t *match_data)
{
    njs_int_t            ret;
    njs_trace_handler_t  handler;

    handler = vm->trace.handler;
    vm->trace.handler = njs_regexp_match_trace_handler;

    ret = njs_regex_match(regex, subject, off, len, match_data,
                          vm->regex_context);

    vm->trace.handler = handler;

    return ret;
}


static u_char *
njs_regexp_match_trace_handler(njs_trace_t *trace, njs_trace_data_t *td,
    u_char *start)
{
    u_char    *p;
    njs_vm_t  *vm;

    vm = trace->data;

    trace = trace->next;
    p = trace->handler(trace, td, start);

    njs_internal_error(vm, (const char *) start);

    return p;
}


njs_regexp_t *
njs_regexp_alloc(njs_vm_t *vm, njs_regexp_pattern_t *pattern)
{
    njs_regexp_t  *regexp;

    regexp = njs_mp_alloc(vm->mem_pool, sizeof(njs_regexp_t));

    if (njs_fast_path(regexp != NULL)) {
        njs_lvlhsh_init(&regexp->object.hash);
        regexp->object.shared_hash = vm->shared->regexp_instance_hash;
        regexp->object.__proto__ = &vm->prototypes[NJS_OBJ_TYPE_REGEXP].object;
        regexp->object.slots = NULL;
        regexp->object.type = NJS_REGEXP;
        regexp->object.shared = 0;
        regexp->object.extensible = 1;
        regexp->object.fast_array = 0;
        regexp->object.error_data = 0;
        njs_set_number(&regexp->last_index, 0);
        regexp->pattern = pattern;
        njs_string_short_set(&regexp->string, 0, 0);
        return regexp;
    }

    njs_memory_error(vm);

    return NULL;
}


static njs_int_t
njs_regexp_prototype_last_index(njs_vm_t *vm, njs_object_prop_t *unused,
    njs_value_t *value, njs_value_t *setval, njs_value_t *retval)
{
    njs_regexp_t  *regexp;

    regexp = njs_object_proto_lookup(njs_object(value), NJS_REGEXP,
                                     njs_regexp_t);
    if (njs_slow_path(regexp == NULL)) {
        njs_set_undefined(retval);
        return NJS_DECLINED;
    }

    if (setval != NULL) {
        regexp->last_index = *setval;
        *retval  = *setval;

        return NJS_OK;
    }

    *retval = regexp->last_index;
    return NJS_OK;
}


static njs_int_t
njs_regexp_prototype_flags(njs_vm_t *vm, njs_value_t *args,
    njs_uint_t nargs, njs_index_t unused)
{
    u_char       *p;
    njs_int_t    ret;
    njs_value_t  *this, value;
    u_char       dst[3];

    static const njs_value_t  string_global = njs_string("global");
    static const njs_value_t  string_ignore_case = njs_string("ignoreCase");
    static const njs_value_t  string_multiline = njs_string("multiline");

    this = njs_argument(args, 0);
    if (njs_slow_path(!njs_is_object(this))) {
        njs_type_error(vm, "\"this\" argument is not an object");
        return NJS_ERROR;
    }

    p = &dst[0];

    ret = njs_value_property(vm, this, njs_value_arg(&string_global),
                             &value);
    if (njs_slow_path(ret == NJS_ERROR)) {
        return NJS_ERROR;
    }

    if (njs_bool(&value)) {
        *p++ = 'g';
    }

    ret = njs_value_property(vm, this, njs_value_arg(&string_ignore_case),
                             &value);
    if (njs_slow_path(ret == NJS_ERROR)) {
        return NJS_ERROR;
    }

    if (njs_bool(&value)) {
        *p++ = 'i';
    }

    ret = njs_value_property(vm, this, njs_value_arg(&string_multiline),
                             &value);
    if (njs_slow_path(ret == NJS_ERROR)) {
        return NJS_ERROR;
    }

    if (njs_bool(&value)) {
        *p++ = 'm';
    }

    return njs_string_new(vm, &vm->retval, dst, p - dst, p - dst);
}


static njs_int_t
njs_regexp_prototype_flag(njs_vm_t *vm, njs_value_t *args,
    njs_uint_t nargs, njs_index_t flag)
{
    unsigned              yn;
    njs_value_t           *this;
    njs_regexp_pattern_t  *pattern;

    this = njs_argument(args, 0);
    if (njs_slow_path(!njs_is_object(this))) {
        njs_type_error(vm, "\"this\" argument is not an object");
        return NJS_ERROR;
    }

    if (njs_slow_path(!njs_is_regexp(this))) {
        if (njs_object(this) == &vm->prototypes[NJS_OBJ_TYPE_REGEXP].object) {
            njs_set_undefined(&vm->retval);
            return NJS_OK;
        }

        njs_type_error(vm, "\"this\" argument is not a regexp");
        return NJS_ERROR;
    }

    pattern = njs_regexp_pattern(this);

    switch (flag) {
    case NJS_REGEXP_GLOBAL:
        yn = pattern->global;
        break;

    case NJS_REGEXP_IGNORE_CASE:
        yn = pattern->ignore_case;
        break;

    case NJS_REGEXP_MULTILINE:
    default:
        yn = pattern->multiline;
        break;
    }

    njs_set_boolean(&vm->retval, yn);

    return NJS_OK;
}


static njs_int_t
njs_regexp_prototype_source(njs_vm_t *vm, njs_value_t *args,
    njs_uint_t nargs, njs_index_t unused)
{
    u_char                *source;
    int32_t               length;
    uint32_t              size;
    njs_value_t           *this;
    njs_regexp_pattern_t  *pattern;

    this = njs_argument(args, 0);
    if (njs_slow_path(!njs_is_object(this))) {
        njs_type_error(vm, "\"this\" argument is not an object");
        return NJS_ERROR;
    }

    if (njs_slow_path(!njs_is_regexp(this))) {
        if (njs_object(this) == &vm->prototypes[NJS_OBJ_TYPE_REGEXP].object) {
            vm->retval = njs_string_empty_regexp;
            return NJS_OK;
        }

        njs_type_error(vm, "\"this\" argument is not a regexp");
        return NJS_ERROR;
    }

    pattern = njs_regexp_pattern(this);
    /* Skip starting "/". */
    source = pattern->source + 1;

    size = njs_strlen(source) - pattern->flags;
    length = njs_utf8_length(source, size);

    return njs_regexp_string_create(vm, &vm->retval, source, size, length);
}


static njs_int_t
njs_regexp_prototype_to_string(njs_vm_t *vm, njs_value_t *args,
    njs_uint_t nargs, njs_index_t unused)
{
    u_char             *p;
    size_t             size, length;
    njs_int_t          ret;
    njs_value_t        *r, source, flags;
    njs_string_prop_t  source_string, flags_string;

    static const njs_value_t  string_source = njs_string("source");
    static const njs_value_t  string_flags = njs_string("flags");

    r = njs_argument(args, 0);

    if (njs_slow_path(!njs_is_object(r))) {
        njs_type_error(vm, "\"this\" argument is not an object");
        return NJS_ERROR;
    }

    ret = njs_value_property(vm, r, njs_value_arg(&string_source),
                             &source);
    if (njs_slow_path(ret == NJS_ERROR)) {
        return NJS_ERROR;
    }

    ret = njs_value_to_string(vm, &source, &source);
    if (njs_slow_path(ret != NJS_OK)) {
        return NJS_ERROR;
    }

    ret = njs_value_property(vm, r, njs_value_arg(&string_flags),
                             &flags);
    if (njs_slow_path(ret == NJS_ERROR)) {
        return NJS_ERROR;
    }

    ret = njs_value_to_string(vm, &flags, &flags);
    if (njs_slow_path(ret != NJS_OK)) {
        return NJS_ERROR;
    }

    (void) njs_string_prop(&source_string, &source);
    (void) njs_string_prop(&flags_string, &flags);

    size = source_string.size + flags_string.size + njs_length("//");
    length = source_string.length + flags_string.length + njs_length("//");

    if (njs_is_byte_string(&source_string)
        || njs_is_byte_string(&flags_string))
    {
        length = 0;
    }

    p = njs_string_alloc(vm, &vm->retval, size, length);
    if (njs_slow_path(p == NULL)) {
        return NJS_ERROR;
    }

    *p++ = '/';
    p = njs_cpymem(p, source_string.start, source_string.size);
    *p++ = '/';
    memcpy(p, flags_string.start, flags_string.size);

    return NJS_OK;
}


njs_int_t
njs_regexp_to_string(njs_vm_t *vm, njs_value_t *retval,
    const njs_value_t *value)
{
    u_char                *p, *source;
    int32_t               length;
    uint32_t              size;
    njs_regexp_pattern_t  *pattern;

    pattern = njs_regexp_pattern(value);
    source = pattern->source;

    size = njs_strlen(source);
    length = njs_utf8_length(source, size);

    length = (length >= 0) ? length: 0;

    p = njs_string_alloc(vm, retval, size, length);
    if (njs_slow_path(p == NULL)) {
        return NJS_ERROR;
    }

    p = njs_cpymem(p, source, size);

    return NJS_OK;
}


static njs_int_t
njs_regexp_prototype_test(njs_vm_t *vm, njs_value_t *args, njs_uint_t nargs,
    njs_index_t unused)
{
    int                     *captures;
    int64_t                 last_index;
    njs_int_t               ret, match;
    njs_uint_t              n;
    njs_regex_t             *regex;
    njs_regexp_t            *regexp;
    njs_value_t             *value, lvalue;
    const njs_value_t       *retval;
    njs_string_prop_t       string;
    njs_regexp_pattern_t    *pattern;
    njs_regex_match_data_t  *match_data;

    if (!njs_is_regexp(njs_arg(args, nargs, 0))) {
        njs_type_error(vm, "\"this\" argument is not a regexp");
        return NJS_ERROR;
    }

    retval = &njs_value_false;

    value = njs_lvalue_arg(&lvalue, args, nargs, 1);

    if (!njs_is_string(value)) {
        ret = njs_value_to_string(vm, value, value);
        if (njs_slow_path(ret != NJS_OK)) {
            return ret;
        }
    }

    (void) njs_string_prop(&string, value);

    n = (string.length != 0);

    regexp = njs_regexp(njs_argument(args, 0));
    pattern = njs_regexp_pattern(&args[0]);

    regex = &pattern->regex[n];
    match_data = vm->single_match_data;

    if (njs_regex_is_valid(regex)) {
        if (njs_regex_backrefs(regex) != 0) {
            match_data = njs_regex_match_data(regex, vm->regex_context);
            if (njs_slow_path(match_data == NULL)) {
                njs_memory_error(vm);
                return NJS_ERROR;
            }
        }

        match = njs_regexp_match(vm, regex, string.start, 0, string.size,
                                 match_data);
        if (match >= 0) {
            retval = &njs_value_true;

        } else if (match != NJS_REGEX_NOMATCH) {
            ret = NJS_ERROR;
            goto done;
        }

        if (pattern->global) {
            ret = njs_value_to_length(vm, &regexp->last_index, &last_index);
            if (njs_slow_path(ret != NJS_OK)) {
                return NJS_ERROR;
            }

            if (match >= 0) {
                captures = njs_regex_captures(match_data);
                last_index += captures[1];

            } else {
                last_index = 0;
            }

            njs_set_number(&regexp->last_index, last_index);
        }
    }

    ret = NJS_OK;

    vm->retval = *retval;

done:

    if (match_data != vm->single_match_data) {
        njs_regex_match_data_free(match_data, vm->regex_context);
    }

    return ret;
}


/**
 * TODO: sticky, unicode flags.
 */
static njs_int_t
njs_regexp_builtin_exec(njs_vm_t *vm, njs_value_t *r, njs_value_t *s,
    njs_value_t *retval)
{
    size_t                  length, offset;
    int64_t                 last_index;
    njs_int_t               ret;
    njs_array_t             *result;
    njs_regexp_t            *regexp;
    njs_string_prop_t       string;
    njs_regexp_utf8_t       type;
    njs_regexp_pattern_t    *pattern;
    njs_regex_match_data_t  *match_data;

    regexp = njs_regexp(r);
    regexp->string = *s;
    pattern = regexp->pattern;

    ret = njs_value_to_length(vm, &regexp->last_index, &last_index);
    if (njs_slow_path(ret != NJS_OK)) {
        return NJS_ERROR;
    }

    if (!pattern->global) {
        last_index = 0;
    }

    length = njs_string_prop(&string, s);

    if (njs_slow_path((size_t) last_index > length)) {
        goto not_found;
    }

    type = NJS_REGEXP_BYTE;

    if (length != string.size) {
        /* UTF-8 string. */
        type = NJS_REGEXP_UTF8;
    }

    pattern = regexp->pattern;

    if (njs_slow_path(!njs_regex_is_valid(&pattern->regex[type]))) {
        goto not_found;
    }

    match_data = njs_regex_match_data(&pattern->regex[type], vm->regex_context);
    if (njs_slow_path(match_data == NULL)) {
        njs_memory_error(vm);
        return NJS_ERROR;
    }

    if (type != NJS_REGEXP_UTF8) {
        offset = last_index;

    } else {
        /* UTF-8 string. */
        offset = njs_string_offset(string.start, string.start + string.size,
                                   last_index) - string.start;
    }

    ret = njs_regexp_match(vm, &pattern->regex[type], string.start, offset,
                           string.size, match_data);
    if (ret >= 0) {
        result = njs_regexp_exec_result(vm, regexp, type, &string, match_data);
        if (njs_slow_path(result == NULL)) {
            return NJS_ERROR;
        }

        njs_set_array(retval, result);
        return NJS_OK;
    }

    if (njs_slow_path(ret != NJS_REGEX_NOMATCH)) {
        njs_regex_match_data_free(match_data, vm->regex_context);

        return NJS_ERROR;
    }

not_found:

    if (pattern->global) {
        njs_set_number(&regexp->last_index, 0);
    }

    njs_set_null(retval);

    return NJS_OK;
}


static njs_array_t *
njs_regexp_exec_result(njs_vm_t *vm, njs_regexp_t *regexp,
    njs_regexp_utf8_t type, njs_string_prop_t *string,
    njs_regex_match_data_t *match_data)
{
    int                   *captures;
    u_char                *start;
    int32_t               size, length;
    uint32_t              index;
    njs_int_t             ret;
    njs_uint_t            i, n;
    njs_array_t           *array;
    njs_value_t           name;
    njs_object_t          *groups;
    njs_object_prop_t     *prop;
    njs_regexp_group_t    *group;
    njs_lvlhsh_query_t    lhq;
    njs_regexp_pattern_t  *pattern;

    static const njs_value_t  string_index = njs_string("index");
    static const njs_value_t  string_input = njs_string("input");
    static const njs_value_t  string_groups = njs_string("groups");

    pattern = regexp->pattern;
    array = njs_array_alloc(vm, 0, pattern->ncaptures, 0);
    if (njs_slow_path(array == NULL)) {
        goto fail;
    }

    captures = njs_regex_captures(match_data);

    for (i = 0; i < pattern->ncaptures; i++) {
        n = 2 * i;

        if (captures[n] != -1) {
            start = &string->start[captures[n]];
            size = captures[n + 1] - captures[n];

            if (type == NJS_REGEXP_UTF8) {
                length = njs_max(njs_utf8_length(start, size), 0);

            } else {
                length = size;
            }

            ret = njs_regexp_string_create(vm, &array->start[i], start, size,
                                           length);
            if (njs_slow_path(ret != NJS_OK)) {
                goto fail;
            }

        } else {
            njs_set_undefined(&array->start[i]);
        }
    }

    /* FIXME: implement fast CreateDataPropertyOrThrow(). */
    prop = njs_object_prop_alloc(vm, &string_index, &njs_value_undefined, 1);
    if (njs_slow_path(prop == NULL)) {
        goto fail;
    }

    if (type == NJS_REGEXP_UTF8) {
        index = njs_string_index(string, captures[0]);

    } else {
        index = captures[0];
    }

    njs_set_number(&prop->value, index);

    if (pattern->global) {
        if (type == NJS_REGEXP_UTF8) {
            index = njs_string_index(string, captures[1]);

        } else {
            index = captures[1];
        }

        njs_set_number(&regexp->last_index, index);
    }

    lhq.key_hash = NJS_INDEX_HASH;
    lhq.key = njs_str_value("index");
    lhq.replace = 0;
    lhq.value = prop;
    lhq.pool = vm->mem_pool;
    lhq.proto = &njs_object_hash_proto;

    ret = njs_lvlhsh_insert(&array->object.hash, &lhq);
    if (njs_slow_path(ret != NJS_OK)) {
        goto insert_fail;
    }

    prop = njs_object_prop_alloc(vm, &string_input, &regexp->string, 1);
    if (njs_slow_path(prop == NULL)) {
        goto fail;
    }

    lhq.key_hash = NJS_INPUT_HASH;
    lhq.key = njs_str_value("input");
    lhq.value = prop;

    ret = njs_lvlhsh_insert(&array->object.hash, &lhq);
    if (njs_slow_path(ret != NJS_OK)) {
        goto insert_fail;
    }

    prop = njs_object_prop_alloc(vm, &string_groups, &njs_value_undefined, 1);
    if (njs_slow_path(prop == NULL)) {
        goto fail;
    }

    lhq.key_hash = NJS_GROUPS_HASH;
    lhq.key = njs_str_value("groups");
    lhq.value = prop;

    ret = njs_lvlhsh_insert(&array->object.hash, &lhq);
    if (njs_slow_path(ret != NJS_OK)) {
        goto insert_fail;
    }

    if (pattern->ngroups != 0) {
        groups = njs_object_alloc(vm);
        if (njs_slow_path(groups == NULL)) {
            goto fail;
        }

        njs_set_object(&prop->value, groups);

        i = 0;

        do {
            group = &pattern->groups[i];

            ret = njs_string_set(vm, &name, group->name.start,
                                 group->name.length);
            if (njs_slow_path(ret != NJS_OK)) {
                goto fail;
            }

            prop = njs_object_prop_alloc(vm, &name,
                                         &array->start[group->capture], 1);
            if (njs_slow_path(prop == NULL)) {
                goto fail;
            }

            lhq.key_hash = group->hash;
            lhq.key = group->name;
            lhq.value = prop;

            ret = njs_lvlhsh_insert(&groups->hash, &lhq);
            if (njs_slow_path(ret != NJS_OK)) {
                goto insert_fail;
            }

            i++;

        } while (i < pattern->ngroups);
    }

    ret = NJS_OK;
    goto done;

insert_fail:

    njs_internal_error(vm, "lvlhsh insert failed");

fail:

    ret = NJS_ERROR;

done:

    njs_regex_match_data_free(match_data, vm->regex_context);

    return (ret == NJS_OK) ? array : NULL;
}


njs_int_t
njs_regexp_prototype_exec(njs_vm_t *vm, njs_value_t *args, njs_uint_t nargs,
    njs_index_t unused)
{
    njs_int_t    ret;
    njs_value_t  *r, *s;
    njs_value_t  string_lvalue;

    r = njs_argument(args, 0);

    if (njs_slow_path(!njs_is_regexp(r))) {
        njs_type_error(vm, "\"this\" argument is not a regexp");
        return NJS_ERROR;
    }

    s = njs_lvalue_arg(&string_lvalue, args, nargs, 1);

    ret = njs_value_to_string(vm, s, s);
    if (njs_slow_path(ret != NJS_OK)) {
        return ret;
    }

    return njs_regexp_builtin_exec(vm, r, s, &vm->retval);
}


njs_int_t
njs_regexp_exec(njs_vm_t *vm, njs_value_t *r, njs_value_t *s,
    njs_value_t *retval)
{
    njs_int_t    ret;
    njs_value_t  exec;

    static const njs_value_t  string_exec = njs_string("exec");

    ret = njs_value_property(vm, r, njs_value_arg(&string_exec), &exec);
    if (njs_slow_path(ret == NJS_ERROR)) {
        return NJS_ERROR;
    }

    if (njs_is_function(&exec)) {
        ret = njs_function_call(vm, njs_function(&exec), r, s, 1, retval);
        if (njs_slow_path(ret == NJS_ERROR)) {
            return NJS_ERROR;
        }

        if (njs_slow_path(!njs_is_object(retval) && !njs_is_null(retval))) {
            njs_type_error(vm, "unexpected \"%s\" retval in njs_regexp_exec()",
                           njs_type_string(retval->type));
            return NJS_ERROR;
        }

        return NJS_OK;
    }

    if (njs_slow_path(!njs_is_regexp(r))) {
        njs_type_error(vm, "receiver argument is not a regexp");
        return NJS_ERROR;
    }

    return njs_regexp_builtin_exec(vm, r, s, retval);
}


static njs_int_t
njs_regexp_string_create(njs_vm_t *vm, njs_value_t *value, u_char *start,
    uint32_t size, int32_t length)
{
    length = (length >= 0) ? length : 0;

    return njs_string_new(vm, value, start, size, length);
}


static njs_int_t
njs_regexp_prototype_symbol_replace(njs_vm_t *vm, njs_value_t *args,
    njs_uint_t nargs, njs_index_t unused)
{
    u_char             *p;
    int64_t            n, last_index, ncaptures, pos, next_pos, size, length;
    njs_str_t          rep, m;
    njs_int_t          ret;
    njs_arr_t          results;
    njs_chb_t          chain;
    njs_uint_t         i;
    njs_bool_t         global;
    njs_array_t        *array;
    njs_value_t        *arguments, *r, *rx, *string, *replace;
    njs_value_t        s_lvalue, r_lvalue, value, matched, groups, retval;
    njs_function_t     *func_replace;
    njs_string_prop_t  s;

    static const njs_value_t  string_global = njs_string("global");
    static const njs_value_t  string_groups = njs_string("groups");
    static const njs_value_t  string_index = njs_string("index");
    static const njs_value_t  string_lindex = njs_string("lastIndex");

    rx = njs_argument(args, 0);

    if (njs_slow_path(!njs_is_object(rx))) {
        njs_type_error(vm, "\"this\" is not object");
        return NJS_ERROR;
    }

    string = njs_lvalue_arg(&s_lvalue, args, nargs, 1);

    ret = njs_value_to_string(vm, string, string);
    if (njs_slow_path(ret != NJS_OK)) {
        return ret;
    }

    length = njs_string_prop(&s, string);

    rep.start = NULL;
    rep.length = 0;

    replace = njs_lvalue_arg(&r_lvalue, args, nargs, 2);
    func_replace = njs_is_function(replace) ? njs_function(replace) : NULL;

    if (!func_replace) {
        ret = njs_value_to_string(vm, replace, replace);
        if (njs_slow_path(ret != NJS_OK)) {
            return ret;
        }
    }

    ret = njs_value_property(vm, rx, njs_value_arg(&string_global), &value);
    if (njs_slow_path(ret == NJS_ERROR)) {
        return NJS_ERROR;
    }

    global = njs_bool(&value);

    if (global) {
        njs_set_number(&value, 0);
        ret = njs_value_property_set(vm, rx, njs_value_arg(&string_lindex),
                                     &value);
        if (njs_slow_path(ret != NJS_OK)) {
            return NJS_ERROR;
        }
    }

    njs_chb_init(&chain, vm->mem_pool);

    results.separate = 0;
    results.pointer = 0;

    r = njs_arr_init(vm->mem_pool, &results, NULL, 4, sizeof(njs_value_t));
    if (njs_slow_path(r == NULL)) {
        return NJS_ERROR;
    }

    for ( ;; ) {
        r = njs_arr_add(&results);
        if (njs_slow_path(r == NULL)) {
            ret = NJS_ERROR;
            goto exception;
        }

        ret = njs_regexp_exec(vm, rx, string, r);
        if (njs_slow_path(ret != NJS_OK)) {
            goto exception;
        }

        if (njs_is_null(r) || !global) {
            break;
        }

        if (njs_fast_path(njs_is_fast_array(r) && njs_array_len(r) != 0)) {
            value = njs_array_start(r)[0];

        } else {
            ret = njs_value_property_i64(vm, r, 0, &value);
            if (njs_slow_path(ret == NJS_ERROR)) {
                goto exception;
            }
        }

        ret = njs_value_to_string(vm, &value, &value);
        if (njs_slow_path(ret != NJS_OK)) {
            goto exception;
        }

        if (njs_string_length(&value) != 0) {
            continue;
        }

        ret = njs_value_property(vm, rx, njs_value_arg(&string_lindex), &value);
        if (njs_slow_path(ret == NJS_ERROR)) {
            goto exception;
        }

        ret = njs_value_to_length(vm, &value, &last_index);
        if (njs_slow_path(ret != NJS_OK)) {
            goto exception;
        }

        njs_set_number(&value, last_index + 1);
        ret = njs_value_property_set(vm, rx, njs_value_arg(&string_lindex),
                                     &value);
        if (njs_slow_path(ret != NJS_OK)) {
            goto exception;
        }
    }

    i = 0;
    pos = 0;
    next_pos = 0;

    while (i < results.items) {
        r = njs_arr_item(&results, i++);

        if (njs_slow_path(njs_is_null(r))) {
            break;
        }

        ret = njs_value_property_i64(vm, r, 0, &matched);
        if (njs_slow_path(ret == NJS_ERROR)) {
            goto exception;
        }

        ret = njs_value_to_string(vm, &matched, &matched);
        if (njs_slow_path(ret != NJS_OK)) {
            goto exception;
        }

        ret = njs_value_property(vm, r, njs_value_arg(&string_index), &value);
        if (njs_slow_path(ret == NJS_ERROR)) {
            goto exception;
        }

        ret = njs_value_to_integer(vm, &value, &pos);
        if (njs_slow_path(ret != NJS_OK)) {
            goto exception;
        }

        if ((size_t) length != s.size) {
            /* UTF-8 string. */
            pos = njs_string_offset(s.start, s.start + s.size, pos) - s.start;
        }

        pos = njs_max(njs_min(pos, (int64_t) s.size), 0);

        if (njs_fast_path(njs_is_fast_array(r) && njs_array_len(r) != 0)) {
            array = njs_array(r);

            arguments = array->start;
            arguments[0] = matched;
            ncaptures = njs_max((int64_t) array->length - 1, 0);

            for (n = 1; n <= ncaptures; n++) {
                if (njs_is_undefined(&arguments[n])) {
                    continue;
                }

                ret = njs_value_to_string(vm, &arguments[n], &arguments[n]);
                if (njs_slow_path(ret == NJS_ERROR)) {
                    goto exception;
                }
            }

        } else {
            ret = njs_object_length(vm, r, &ncaptures);
            if (njs_slow_path(ret != NJS_OK)) {
                goto exception;
            }

            ncaptures = njs_max(ncaptures - 1, 0);

            array = njs_array_alloc(vm, 0, ncaptures + 1, 0);
            if (njs_slow_path(array == NULL)) {
                goto exception;
            }

            arguments = array->start;
            arguments[0] = matched;

            for (n = 1; n <= ncaptures; n++) {
                ret = njs_value_property_i64(vm, r, n, &arguments[n]);
                if (njs_slow_path(ret == NJS_ERROR)) {
                    goto exception;
                }

                if (njs_is_undefined(&arguments[n])) {
                    continue;
                }

                ret = njs_value_to_string(vm, &arguments[n], &arguments[n]);
                if (njs_slow_path(ret == NJS_ERROR)) {
                    goto exception;
                }
            }
        }

        ret = njs_value_property(vm, r, njs_value_arg(&string_groups), &groups);
        if (njs_slow_path(ret == NJS_ERROR)) {
            goto exception;
        }

        if (!func_replace) {
            if (njs_is_defined(&groups)) {
                ret = njs_value_to_object(vm, &groups);
                if (njs_slow_path(ret != NJS_OK)) {
                    return ret;
                }
            }

            ret = njs_string_get_substitution(vm, &matched, string, pos,
                                              arguments, ncaptures, &groups,
                                              replace, &retval);

        } else {
            ret = njs_array_expand(vm, array, 0,
                                   njs_is_defined(&groups) ? 3 : 2);
            if (njs_slow_path(ret != NJS_OK)) {
                goto exception;
            }

            arguments = array->start;
            njs_set_number(&arguments[n++], pos);
            arguments[n++] = *string;

            if (njs_is_defined(&groups)) {
                arguments[n++] = groups;
            }

            ret = njs_function_call(vm, func_replace,
                                    njs_value_arg(&njs_value_undefined),
                                    arguments, n, &retval);
        }

        if (njs_slow_path(ret == NJS_ERROR)) {
            return NJS_ERROR;
        }

        ret = njs_value_to_string(vm, &retval, &retval);
        if (njs_slow_path(ret != NJS_OK)) {
            goto exception;
        }

        if (pos >= next_pos) {
            njs_chb_append(&chain, &s.start[next_pos], pos - next_pos);

            njs_string_get(&retval, &rep);
            njs_chb_append_str(&chain, &rep);

            njs_string_get(&matched, &m);

            next_pos = pos + (int64_t) m.length;
        }
    }

    if (next_pos < (int64_t) s.size) {
        njs_chb_append(&chain, &s.start[next_pos], s.size - next_pos);
    }

    size = njs_chb_size(&chain);
    if (njs_slow_path(size < 0)) {
        njs_memory_error(vm);
        ret = NJS_ERROR;
        goto exception;
    }

    length = njs_chb_utf8_length(&chain);

    p = njs_string_alloc(vm, &vm->retval, size, length);
    if (njs_slow_path(p == NULL)) {
        ret = NJS_ERROR;
        goto exception;
    }

    njs_chb_join_to(&chain, p);

    ret = NJS_OK;

exception:

    njs_chb_destroy(&chain);
    njs_arr_destroy(&results);

    return ret;
}




static const njs_object_prop_t  njs_regexp_constructor_properties[] =
{
    {
        .type = NJS_PROPERTY,
        .name = njs_string("name"),
        .value = njs_string("RegExp"),
        .configurable = 1,
    },

    {
        .type = NJS_PROPERTY,
        .name = njs_string("length"),
        .value = njs_value(NJS_NUMBER, 1, 2.0),
        .configurable = 1,
    },

    {
        .type = NJS_PROPERTY_HANDLER,
        .name = njs_string("prototype"),
        .value = njs_prop_handler(njs_object_prototype_create),
    },
};


const njs_object_init_t  njs_regexp_constructor_init = {
    njs_regexp_constructor_properties,
    njs_nitems(njs_regexp_constructor_properties),
};


static const njs_object_prop_t  njs_regexp_prototype_properties[] =
{
    {
        .type = NJS_PROPERTY_HANDLER,
        .name = njs_string("constructor"),
        .value = njs_prop_handler(njs_object_prototype_create_constructor),
        .writable = 1,
        .configurable = 1,
    },

    {
        .type = NJS_PROPERTY,
        .name = njs_string("flags"),
        .value = njs_value(NJS_INVALID, 1, NAN),
        .getter = njs_native_function(njs_regexp_prototype_flags, 0),
        .setter = njs_value(NJS_UNDEFINED, 0, NAN),
        .writable = NJS_ATTRIBUTE_UNSET,
        .configurable = 1,
        .enumerable = 0,
    },

    {
        .type = NJS_PROPERTY,
        .name = njs_string("global"),
        .value = njs_value(NJS_INVALID, 1, NAN),
        .getter = njs_native_function2(njs_regexp_prototype_flag, 0,
                                       NJS_REGEXP_GLOBAL),
        .setter = njs_value(NJS_UNDEFINED, 0, NAN),
        .writable = NJS_ATTRIBUTE_UNSET,
        .configurable = 1,
        .enumerable = 0,
    },

    {
        .type = NJS_PROPERTY,
        .name = njs_string("ignoreCase"),
        .value = njs_value(NJS_INVALID, 1, NAN),
        .getter = njs_native_function2(njs_regexp_prototype_flag, 0,
                                       NJS_REGEXP_IGNORE_CASE),
        .setter = njs_value(NJS_UNDEFINED, 0, NAN),
        .writable = NJS_ATTRIBUTE_UNSET,
        .configurable = 1,
        .enumerable = 0,
    },

    {
        .type = NJS_PROPERTY,
        .name = njs_string("multiline"),
        .value = njs_value(NJS_INVALID, 1, NAN),
        .getter = njs_native_function2(njs_regexp_prototype_flag, 0,
                                       NJS_REGEXP_MULTILINE),
        .setter = njs_value(NJS_UNDEFINED, 0, NAN),
        .writable = NJS_ATTRIBUTE_UNSET,
        .configurable = 1,
        .enumerable = 0,
    },

    {
        .type = NJS_PROPERTY,
        .name = njs_string("source"),
        .value = njs_value(NJS_INVALID, 1, NAN),
        .getter = njs_native_function(njs_regexp_prototype_source, 0),
        .setter = njs_value(NJS_UNDEFINED, 0, NAN),
        .writable = NJS_ATTRIBUTE_UNSET,
        .configurable = 1,
        .enumerable = 0,
    },

    {
        .type = NJS_PROPERTY,
        .name = njs_string("toString"),
        .value = njs_native_function(njs_regexp_prototype_to_string, 0),
        .writable = 1,
        .configurable = 1,
    },

    {
        .type = NJS_PROPERTY,
        .name = njs_string("test"),
        .value = njs_native_function(njs_regexp_prototype_test, 1),
        .writable = 1,
        .configurable = 1,
    },

    {
        .type = NJS_PROPERTY,
        .name = njs_string("exec"),
        .value = njs_native_function(njs_regexp_prototype_exec, 1),
        .writable = 1,
        .configurable = 1,
    },

    {
        .type = NJS_PROPERTY,
        .name = njs_wellknown_symbol(NJS_SYMBOL_REPLACE),
        .value = njs_native_function(njs_regexp_prototype_symbol_replace, 2),
        .writable = 1,
        .configurable = 1,
    },
};


const njs_object_prop_t  njs_regexp_instance_properties[] =
{
    {
        .type = NJS_PROPERTY_HANDLER,
        .name = njs_string("lastIndex"),
        .value = njs_prop_handler(njs_regexp_prototype_last_index),
        .writable = 1,
    },
};


const njs_object_init_t  njs_regexp_instance_init = {
    njs_regexp_instance_properties,
    njs_nitems(njs_regexp_instance_properties),
};


const njs_object_init_t  njs_regexp_prototype_init = {
    njs_regexp_prototype_properties,
    njs_nitems(njs_regexp_prototype_properties),
};


const njs_object_type_init_t  njs_regexp_type_init = {
   .constructor = njs_native_ctor(njs_regexp_constructor, 2, 0),
   .constructor_props = &njs_regexp_constructor_init,
   .prototype_props = &njs_regexp_prototype_init,
   .prototype_value = { .object = { .type = NJS_OBJECT } },
};
