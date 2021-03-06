/*
 * Copyright (C) 2009-2013 MongoDB, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * This file contains C implementations of some of the functions needed by the
 * bson module. If possible, these implementations should be used to speed up
 * BSON encoding and decoding.
 */

#include "ruby.h"
#include "version.h"

#if (defined(_WIN16) || defined(_WIN32) || defined(_WIN64)) && !defined(__WINDOWS__)
# define __WINDOWS__
# include <winsock2.h>
#else
# include <arpa/inet.h>
#endif

/* Ensure compatibility with early releases of Ruby 1.8.5 */
#ifndef RSTRING_PTR
#  define RSTRING_PTR(v) RSTRING(v)->ptr
#endif

#ifndef RSTRING_LEN
#  define RSTRING_LEN(v) RSTRING(v)->len
#endif
#ifndef RSTRING_LENINT
#  define RSTRING_LENINT(v) (int)(RSTRING_LEN(v))
#endif

#ifndef RARRAY_LEN
#  define RARRAY_LEN(v) RARRAY(v)->len
#endif
#ifndef RARRAY_LENINT
#  define RARRAY_LENINT(v) (int)(RARRAY_LEN(v))
#endif

#if HAVE_RUBY_ST_H
#include "ruby/st.h"
#endif
#if HAVE_ST_H
#include "st.h"
#endif

#if HAVE_RUBY_REGEX_H
#include "ruby/regex.h"
#endif
#if HAVE_REGEX_H
#include "regex.h"
#endif

#include <string.h>
#include <math.h>
#include <unistd.h>
#include <time.h>

#include "version.h"
#include "bson_buffer.h"
#include "encoding_helpers.h"

#define SAFE_WRITE(buffer, data, size)                                  \
    if (bson_buffer_write((buffer), (data), (size)) != 0)                    \
        rb_raise(rb_eNoMemError, "failed to allocate memory in bson_buffer.c")

#define SAFE_WRITE_AT_POS(buffer, position, data, size)                 \
    if (bson_buffer_write_at_position((buffer), (position), (data), (size)) != 0) \
        rb_raise(rb_eRuntimeError, "invalid write at position in bson_buffer.c")

#define MAX_HOSTNAME_LENGTH 256

static ID element_assignment_method;
static ID unpack_method;
static ID utc_method;
static ID lt_operator;
static ID gt_operator;

static VALUE Binary;
static VALUE ObjectId;
static VALUE DBRef;
static VALUE Code;
static VALUE MinKey;
static VALUE MaxKey;
static VALUE Timestamp;
static VALUE Regexp;
static VALUE BSONRegex;
static VALUE BSONRegex_IGNORECASE;
static VALUE BSONRegex_EXTENDED;
static VALUE BSONRegex_MULTILINE;
static VALUE BSONRegex_DOTALL;
static VALUE BSONRegex_LOCALE_DEPENDENT;
static VALUE BSONRegex_UNICODE;
static VALUE OrderedHash;
static VALUE InvalidKeyName;
static VALUE InvalidStringEncoding;
static VALUE InvalidDocument;
static VALUE InvalidObjectId;
static VALUE DigestMD5;
static VALUE RB_HASH;

static int max_bson_size;

struct deserialize_opts {
    int compile_regex;
};

#if HAVE_RUBY_ENCODING_H
#include "ruby/encoding.h"
#define STR_NEW(p,n)                                                    \
    ({                                                                  \
        VALUE _str = rb_enc_str_new((p), (n), rb_utf8_encoding());      \
        rb_encoding* internal_encoding = rb_default_internal_encoding();\
        if (internal_encoding) {                                        \
            _str = rb_str_export_to_enc(_str, internal_encoding);       \
        }                                                               \
        _str;                                                           \
    })
#else
#define STR_NEW(p,n) rb_str_new((p), (n))
#endif

static void write_utf8(bson_buffer_t buffer, VALUE string, int allow_null) {
    result_t status = validate_utf8_encoding(
        (const char*)RSTRING_PTR(string), RSTRING_LEN(string), allow_null);

    if (status == HAS_NULL) {
        bson_buffer_free(buffer);
        rb_raise(InvalidDocument, "Key names / regex patterns must not contain the NULL byte");
    } else if (status == INVALID_UTF8) {
        bson_buffer_free(buffer);
        rb_raise(InvalidStringEncoding, "String not valid UTF-8");
    }
    SAFE_WRITE(buffer, RSTRING_PTR(string), (int)RSTRING_LEN(string));
}

// this sucks. but for some reason these moved around between 1.8 and 1.9
#ifdef ONIGURUMA_H
#define IGNORECASE ONIG_OPTION_IGNORECASE
#define MULTILINE ONIG_OPTION_MULTILINE
#define EXTENDED ONIG_OPTION_EXTEND
#else
#define IGNORECASE RE_OPTION_IGNORECASE
#define MULTILINE RE_OPTION_MULTILINE
#define EXTENDED RE_OPTION_EXTENDED
#endif

/* TODO maybe we can use something more portable like vsnprintf instead
 * of this hack. And share it with the Python extension ;) */
/* If we don't have ASPRINTF, there are two possibilities:
 * either use _scprintf and _snprintf on for Windows or
 * use snprintf for solaris. */
#ifndef HAVE_ASPRINTF
#ifdef _WIN32 || _MSC_VER
#define INT2STRING(buffer, i)                   \
    {                                           \
        int vslength = _scprintf("%d", i) + 1;  \
        *buffer = malloc(vslength);             \
        if (buffer == NULL) {                   \
            rb_raise(rb_eNoMemError, "failed to allocate memory in INT2STRING");  \
        }                                       \
        _snprintf(*buffer, vslength, "%d", i);  \
    }
#define FREE_INTSTRING(buffer) free(buffer)
#else
#define INT2STRING(buffer, i)                           \
    {                                                   \
        int vslength = snprintf(NULL, 0, "%d", i) + 1;  \
        *buffer = malloc(vslength);             \
        if (buffer == NULL) {                   \
            rb_raise(rb_eNoMemError, "failed to allocate memory in INT2STRING");  \
        }                                       \
        snprintf(*buffer, vslength, "%d", i);   \
    }
#define FREE_INTSTRING(buffer) free(buffer)
#endif
#else
#define INT2STRING(buffer, i)                   \
    {                                           \
        int length = asprintf(buffer, "%d", i); \
        if (length == -1) {                     \
            rb_raise(rb_eNoMemError, "failed to allocate memory in INT2STRING");  \
        }                                       \
    }
#ifdef USING_SYSTEM_ALLOCATOR_LIBRARY /* Ruby Enterprise Edition with tcmalloc */
#define FREE_INTSTRING(buffer) system_free(buffer)
#else
#define FREE_INTSTRING(buffer) free(buffer)
#endif
#endif

#ifndef RREGEXP_SRC
#define RREGEXP_SRC(r) rb_str_new(RREGEXP((r))->str, RREGEXP((r))->len)
#endif

// rubinius compatibility
#ifndef RREGEXP_OPTIONS
#define RREGEXP_OPTIONS(r) RREGEXP(value)->ptr->options
#endif

static char zero = 0;
static char one = 1;

static char hostname_digest[17];
static unsigned int object_id_inc = 0;

static int cmp_char(const void* a, const void* b) {
    return *(char*)a - *(char*)b;
}

static void write_doc(bson_buffer_t buffer, VALUE hash, VALUE check_keys, VALUE move_id);
static int write_element_with_id(VALUE key, VALUE value, VALUE extra);
static int write_element_without_id(VALUE key, VALUE value, VALUE extra);
static VALUE elements_to_hash(const char* buffer, int max, struct deserialize_opts * opts);

static VALUE pack_extra(bson_buffer_t buffer, VALUE check_keys) {
    return rb_ary_new3(2, LL2NUM((long long)buffer), check_keys);
}

static void write_name_and_type(bson_buffer_t buffer, VALUE name, char type) {
    SAFE_WRITE(buffer, &type, 1);
    write_utf8(buffer, name, 0);
    SAFE_WRITE(buffer, &zero, 1);
}

static void serialize_regex(bson_buffer_t buffer, VALUE key, VALUE pattern, long flags, VALUE value, int native) {

    VALUE has_extra;

    write_name_and_type(buffer, key, 0x0B);

    write_utf8(buffer, pattern, 0);
    SAFE_WRITE(buffer, &zero, 1);

    if (native == 1) {
        // Ruby regular expressions always use multiline mode
        char multiline = 'm';
        SAFE_WRITE(buffer, &multiline, 1);

        if (flags & IGNORECASE) {
            char ignorecase = 'i';
            SAFE_WRITE(buffer, &ignorecase, 1);
        }

        // dotall on the server is multiline in Ruby
        if (flags & MULTILINE) {
            char dotall = 's';
            SAFE_WRITE(buffer, &dotall, 1);
        }

        if (flags & EXTENDED) {
            char extended = 'x';
            SAFE_WRITE(buffer, &extended, 1);
        }
    }
    else {
        if (flags & BSONRegex_IGNORECASE) {
            char ignorecase = 'i';
            SAFE_WRITE(buffer, &ignorecase, 1);
        }

        if (flags & BSONRegex_LOCALE_DEPENDENT) {
            char locale_dependent = 'l';
            SAFE_WRITE(buffer, &locale_dependent, 1);
        }

        if (flags & BSONRegex_MULTILINE) {
            char multiline = 'm';
            SAFE_WRITE(buffer, &multiline, 1);
        }

        if (flags & BSONRegex_DOTALL) {
            char dotall = 's';
            SAFE_WRITE(buffer, &dotall, 1);
        }

        if (flags & BSONRegex_UNICODE) {
            char unicode = 'u';
            SAFE_WRITE(buffer, &unicode, 1);
        }

        if (flags & BSONRegex_EXTENDED) {
            char extended = 'x';
            SAFE_WRITE(buffer, &extended, 1);
        }
    }

    has_extra = rb_funcall(value, rb_intern("respond_to?"), 1, rb_str_new2("extra_options_str"));
    if (TYPE(has_extra) == T_TRUE) {
         VALUE extra = rb_funcall(value, rb_intern("extra_options_str"), 0);
         bson_buffer_position old_position = bson_buffer_get_position(buffer);
         SAFE_WRITE(buffer, RSTRING_PTR(extra), RSTRING_LENINT(extra));
         qsort(bson_buffer_get_buffer(buffer) + old_position, RSTRING_LEN(extra), sizeof(char), cmp_char);
    }
    SAFE_WRITE(buffer, &zero, 1);

}

static int write_element(VALUE key, VALUE value, VALUE extra, int allow_id) {
    bson_buffer_t buffer = (bson_buffer_t)NUM2LL(rb_ary_entry(extra, 0));
    VALUE check_keys = rb_ary_entry(extra, 1);

    if (TYPE(key) == T_SYMBOL) {
        // TODO better way to do this... ?
        key = rb_str_new2(rb_id2name(SYM2ID(key)));
    }

    if (TYPE(key) != T_STRING) {
        bson_buffer_free(buffer);
        rb_raise(rb_eTypeError, "keys must be strings or symbols");
    }

    if (allow_id == 0 && strcmp("_id", RSTRING_PTR(key)) == 0) {
        return ST_CONTINUE;
    }

    if (check_keys == Qtrue) {
        int i;
        if (RSTRING_LEN(key) > 0 && RSTRING_PTR(key)[0] == '$') {
            bson_buffer_free(buffer);
            rb_raise(InvalidKeyName, "key %s must not start with '$'", RSTRING_PTR(key));
        }
        for (i = 0; i < RSTRING_LEN(key); i++) {
            if (RSTRING_PTR(key)[i] == '.') {
                bson_buffer_free(buffer);
                rb_raise(InvalidKeyName, "key %s must not contain '.'", RSTRING_PTR(key));
            }
        }
    }

    switch(TYPE(value)) {
    case T_BIGNUM:
        {
            if (rb_funcall(value, gt_operator, 1, LL2NUM(9223372036854775807LL)) == Qtrue ||
                rb_funcall(value, lt_operator, 1, LL2NUM(-9223372036854775808ULL)) == Qtrue) {
                bson_buffer_free(buffer);
                rb_raise(rb_eRangeError, "MongoDB can only handle 8-byte ints");
            }
        }
        // NOTE: falls through to T_FIXNUM code
    case T_FIXNUM:
        {
            long long ll_value;
            ll_value = NUM2LL(value);

            if (ll_value > 2147483647LL ||
                ll_value < -2147483648LL) {
                write_name_and_type(buffer, key, 0x12);
                SAFE_WRITE(buffer, (char*)&ll_value, 8);
            } else {
                int int_value;
                write_name_and_type(buffer, key, 0x10);
                int_value = (int)ll_value;
                SAFE_WRITE(buffer, (char*)&int_value, 4);
            }
            break;
        }
    case T_TRUE:
        {
            write_name_and_type(buffer, key, 0x08);
            SAFE_WRITE(buffer, &one, 1);
            break;
        }
    case T_FALSE:
        {
            write_name_and_type(buffer, key, 0x08);
            SAFE_WRITE(buffer, &zero, 1);
            break;
        }
    case T_FLOAT:
        {
            double d = NUM2DBL(value);
            write_name_and_type(buffer, key, 0x01);
            SAFE_WRITE(buffer, (char*)&d, 8);
            break;
        }
    case T_NIL:
        {
            write_name_and_type(buffer, key, 0x0A);
            break;
        }
    case T_HASH:
        {
            write_name_and_type(buffer, key, 0x03);
            write_doc(buffer, value, check_keys, Qfalse);
            break;
        }
    case T_ARRAY:
        {
            bson_buffer_position length_location, start_position, obj_length;
            int items, i;

            write_name_and_type(buffer, key, 0x04);
            start_position = bson_buffer_get_position(buffer);

            // save space for length
            length_location = bson_buffer_save_space(buffer, 4);
            if (length_location == -1) {
                rb_raise(rb_eNoMemError, "failed to allocate memory in buffer.c");
            }

            items = RARRAY_LENINT(value);
            for(i = 0; i < items; i++) {
                char* name;
                VALUE key;
                INT2STRING(&name, i);
                key = rb_str_new2(name);
                write_element_with_id(key, rb_ary_entry(value, i), pack_extra(buffer, check_keys));
                FREE_INTSTRING(name);
            }

            // write null byte and fill in length
            SAFE_WRITE(buffer, &zero, 1);
            obj_length = bson_buffer_get_position(buffer) - start_position;
            SAFE_WRITE_AT_POS(buffer, length_location, (const char*)&obj_length, 4);
            break;
        }
    case T_STRING:
        {
            int length;
            write_name_and_type(buffer, key, 0x02);
            length = RSTRING_LENINT(value) + 1;
            SAFE_WRITE(buffer, (char*)&length, 4);
            write_utf8(buffer, value, 1);
            SAFE_WRITE(buffer, &zero, 1);
            break;
        }
    case T_SYMBOL:
        {
            const char* str_value = rb_id2name(SYM2ID(value));
            int length = (int)strlen(str_value) + 1;
            write_name_and_type(buffer, key, 0x0E);
            SAFE_WRITE(buffer, (char*)&length, 4);
            SAFE_WRITE(buffer, str_value, length);
            break;
        }
    case T_OBJECT:
        {
            // TODO there has to be a better way to do these checks...
            const char* cls = rb_obj_classname(value);
            if (strcmp(cls, "BSON::Binary") == 0 ||
                strcmp(cls, "ByteBuffer") == 0) {
                VALUE string_data = rb_funcall(value, rb_intern("to_s"), 0);
                int length = RSTRING_LENINT(string_data);
                const char subtype = strcmp(cls, "ByteBuffer") ?
                    (const char)FIX2INT(rb_funcall(value, rb_intern("subtype"), 0)) : 2;
                write_name_and_type(buffer, key, 0x05);
                if (subtype == 2) {
                    const int other_length = length + 4;
                    SAFE_WRITE(buffer, (const char*)&other_length, 4);
                    SAFE_WRITE(buffer, &subtype, 1);
                }
                SAFE_WRITE(buffer, (const char*)&length, 4);
                if (subtype != 2) {
                    SAFE_WRITE(buffer, &subtype, 1);
                }
                SAFE_WRITE(buffer, RSTRING_PTR(string_data), length);
                break;
            }
            if (strcmp(cls, "BSON::ObjectId") == 0) {
                int i;
                VALUE as_array = rb_funcall(value, rb_intern("to_a"), 0);
                write_name_and_type(buffer, key, 0x07);
                for (i = 0; i < 12; i++) {
                    char byte = (char)FIX2INT(rb_ary_entry(as_array, i));
                    SAFE_WRITE(buffer, &byte, 1);
                }
                break;
            }
            if (strcmp(cls, "BSON::DBRef") == 0) {
                bson_buffer_position length_location, start_position, obj_length;
                VALUE ns, oid;
                write_name_and_type(buffer, key, 0x03);

                start_position = bson_buffer_get_position(buffer);

                // save space for length
                length_location = bson_buffer_save_space(buffer, 4);
                if (length_location == -1) {
                    rb_raise(rb_eNoMemError, "failed to allocate memory in buffer.c");
                }

                ns = rb_funcall(value, rb_intern("namespace"), 0);
                write_element_with_id(rb_str_new2("$ref"), ns, pack_extra(buffer, Qfalse));
                oid = rb_funcall(value, rb_intern("object_id"), 0);
                write_element_with_id(rb_str_new2("$id"), oid, pack_extra(buffer, Qfalse));

                // write null byte and fill in length
                SAFE_WRITE(buffer, &zero, 1);
                obj_length = bson_buffer_get_position(buffer) - start_position;
                SAFE_WRITE_AT_POS(buffer, length_location, (const char*)&obj_length, 4);
                break;
            }
            if (strcmp(cls, "BSON::Code") == 0) {
                bson_buffer_position length_location, start_position, total_length;
                int length;
                VALUE code_str;
                write_name_and_type(buffer, key, 0x0F);

                start_position = bson_buffer_get_position(buffer);
                length_location = bson_buffer_save_space(buffer, 4);
                if (length_location == -1) {
                    rb_raise(rb_eNoMemError, "failed to allocate memory in buffer.c");
                }

                code_str = rb_funcall(value, rb_intern("code"), 0);
                length = RSTRING_LENINT(code_str) + 1;
                SAFE_WRITE(buffer, (char*)&length, 4);
                SAFE_WRITE(buffer, RSTRING_PTR(code_str), length - 1);
                SAFE_WRITE(buffer, &zero, 1);
                write_doc(buffer, rb_funcall(value, rb_intern("scope"), 0), Qfalse, Qfalse);

                total_length = bson_buffer_get_position(buffer) - start_position;
                SAFE_WRITE_AT_POS(buffer, length_location, (const char*)&total_length, 4);
                break;
            }
            if (strcmp(cls, "BSON::MaxKey") == 0) {
                write_name_and_type(buffer, key, 0x7f);
                break;
            }
            if (strcmp(cls, "BSON::MinKey") == 0) {
                write_name_and_type(buffer, key, 0xff);
                break;
            }
            if (strcmp(cls, "BSON::Timestamp") == 0) {
                unsigned int seconds;
                unsigned int increment;

                write_name_and_type(buffer, key, 0x11);

                seconds = NUM2UINT(
                    rb_funcall(value, rb_intern("seconds"), 0));
                increment = NUM2UINT(
                    rb_funcall(value, rb_intern("increment"), 0));

                SAFE_WRITE(buffer, (const char*)&increment, 4);
                SAFE_WRITE(buffer, (const char*)&seconds, 4);
                break;
            }
            if (strcmp(cls, "DateTime") == 0 || strcmp(cls, "Date") == 0 || strcmp(cls, "ActiveSupport::TimeWithZone") == 0) {
                bson_buffer_free(buffer);
                rb_raise(InvalidDocument, "%s is not currently supported; use a UTC Time instance instead.", cls);
                break;
            }
            if(strcmp(cls, "Complex") == 0 || strcmp(cls, "Rational") == 0 || strcmp(cls, "BigDecimal") == 0) {
                bson_buffer_free(buffer);
                rb_raise(InvalidDocument, "Cannot serialize the Numeric type %s as BSON; only Bignum, Fixnum, and Float are supported.", cls);
                break;
            }
            if (strcmp(cls, "ActiveSupport::Multibyte::Chars") == 0) {
                int length;
                VALUE str = StringValue(value);
                write_name_and_type(buffer, key, 0x02);
                length = RSTRING_LENINT(str) + 1;
                SAFE_WRITE(buffer, (char*)&length, 4);
                write_utf8(buffer, str, 1);
                SAFE_WRITE(buffer, &zero, 1);
                break;
            }
            if (strcmp(cls, "BSON::Regex") == 0) {
                serialize_regex(buffer, key, rb_funcall(value, rb_intern("pattern"), 0),
                    FIX2INT(rb_funcall(value, rb_intern("options"), 0)), value, 0);
                break;
            }
            bson_buffer_free(buffer);
            rb_raise(InvalidDocument, "Cannot serialize an object of class %s into BSON.", cls);
            break;
        }
    case T_DATA:
        {
            const char* cls = rb_obj_classname(value);
            if (strcmp(cls, "Time") == 0) {
                double t = NUM2DBL(rb_funcall(value, rb_intern("to_f"), 0));
                long long time_since_epoch = (long long)round(t * 1000);
                write_name_and_type(buffer, key, 0x09);
                SAFE_WRITE(buffer, (const char*)&time_since_epoch, 8);
                break;
            }
            // Date classes are TYPE T_DATA in Ruby >= 1.9.3
            if (strcmp(cls, "DateTime") == 0 || strcmp(cls, "Date") == 0 || strcmp(cls, "ActiveSupport::TimeWithZone") == 0) {
                bson_buffer_free(buffer);
                rb_raise(InvalidDocument, "%s is not currently supported; use a UTC Time instance instead.", cls);
                break;
            }
            if(strcmp(cls, "BigDecimal") == 0) {
                bson_buffer_free(buffer);
                rb_raise(InvalidDocument, "Cannot serialize the Numeric type %s as BSON; only Bignum, Fixnum, and Float are supported.", cls);
                break;
            }
            bson_buffer_free(buffer);
            rb_raise(InvalidDocument, "Cannot serialize an object of class %s into BSON.", cls);
            break;
        }
    case T_REGEXP:
        {
            VALUE pattern = RREGEXP_SRC(value);
            long flags = RREGEXP_OPTIONS(value);
            serialize_regex(buffer, key, pattern, flags, value, 1);
            break;
        }
    default:
        {
            const char* cls = rb_obj_classname(value);
            bson_buffer_free(buffer);
            rb_raise(InvalidDocument, "Cannot serialize an object of class %s (type %d) into BSON.", cls, TYPE(value));
            break;
        }
    }
    return ST_CONTINUE;
}

static int write_element_without_id(VALUE key, VALUE value, VALUE extra) {
    return write_element(key, value, extra, 0);
}

static int write_element_with_id(VALUE key, VALUE value, VALUE extra) {
    return write_element(key, value, extra, 1);
}

static void write_doc(bson_buffer_t buffer, VALUE hash, VALUE check_keys, VALUE move_id) {
    bson_buffer_position start_position = bson_buffer_get_position(buffer);
    bson_buffer_position length_location = bson_buffer_save_space(buffer, 4);
    bson_buffer_position length;
    int allow_id;
    int max_size;
    int (*write_function)(VALUE, VALUE, VALUE) = NULL;
    VALUE id_str = rb_str_new2("_id");
    VALUE id_sym = ID2SYM(rb_intern("_id"));

    if (length_location == -1) {
        rb_raise(rb_eNoMemError, "failed to allocate memory in buffer.c");
    }

    // write '_id' first if move_id is true. then don't allow an id to be written.
    if(move_id == Qtrue) {
        allow_id = 0;
        if (rb_funcall(hash, rb_intern("has_key?"), 1, id_str) == Qtrue) {
            VALUE id = rb_hash_aref(hash, id_str);
            write_element_with_id(id_str, id, pack_extra(buffer, check_keys));
        } else if (rb_funcall(hash, rb_intern("has_key?"), 1, id_sym) == Qtrue) {
            VALUE id = rb_hash_aref(hash, id_sym);
            write_element_with_id(id_sym, id, pack_extra(buffer, check_keys));
        }
    }
    else {
        allow_id = 1;
        // Ensure that hash doesn't contain both '_id' and :_id
        if ((rb_obj_classname(hash), "Hash") == 0) {
            if ((rb_funcall(hash, rb_intern("has_key?"), 1, id_str) == Qtrue) &&
                   (rb_funcall(hash, rb_intern("has_key?"), 1, id_sym) == Qtrue)) {
                      VALUE oid_sym = rb_hash_delete(hash, id_sym);
                      rb_funcall(hash, rb_intern("[]="), 2, id_str, oid_sym);
            }
        }
    }

    if(allow_id == 1) {
        write_function = write_element_with_id;
    }
    else {
        write_function = write_element_without_id;
    }

    // we have to check for an OrderedHash and handle that specially
    if (strcmp(rb_obj_classname(hash), "BSON::OrderedHash") == 0) {
        int i;
        VALUE keys = rb_funcall(hash, rb_intern("keys"), 0);

        for(i = 0; i < RARRAY_LEN(keys); i++) {
            VALUE key = rb_ary_entry(keys, i);
            VALUE value = rb_hash_aref(hash, key);

            write_function(key, value, pack_extra(buffer, check_keys));
        }
    } else if (rb_obj_is_kind_of(hash, RB_HASH) == Qtrue) {
        rb_hash_foreach(hash, write_function, pack_extra(buffer, check_keys));
    } else {
        bson_buffer_free(buffer);
        rb_raise(InvalidDocument, "BSON.serialize takes a Hash but got a %s", rb_obj_classname(hash));
    }

    // write null byte and fill in length
    SAFE_WRITE(buffer, &zero, 1);
    length = bson_buffer_get_position(buffer) - start_position;

    // make sure that length doesn't exceed the max size (determined by server, defaults to 4mb)
    max_size = bson_buffer_get_max_size(buffer);
    if (length > max_size) {
        bson_buffer_free(buffer);
        rb_raise(InvalidDocument,
            "Document too large: This BSON document is limited to %d bytes.",
            max_size);
    }
    SAFE_WRITE_AT_POS(buffer, length_location, (const char*)&length, 4);
}

static VALUE method_serialize(VALUE self, VALUE doc, VALUE check_keys,
    VALUE move_id, VALUE max_size) {

    VALUE result;
    bson_buffer_t buffer = bson_buffer_new();
    if (buffer == NULL) {
        rb_raise(rb_eNoMemError, "failed to allocate memory in buffer.c");
    }
    bson_buffer_set_max_size(buffer, FIX2INT(max_size));

    write_doc(buffer, doc, check_keys, move_id);

    result = rb_str_new(bson_buffer_get_buffer(buffer), bson_buffer_get_position(buffer));
    if (bson_buffer_free(buffer) != 0) {
        rb_raise(rb_eRuntimeError, "failed to free buffer");
    }
    return result;
}

static VALUE get_value(const char* buffer, int* position,
                       unsigned char type, struct deserialize_opts * opts) {
    VALUE value;
    switch (type) {
    case 255:
        {
            value = rb_class_new_instance(0, NULL, MinKey);
            break;
        }
    case 1:
        {
            double d;
            memcpy(&d, buffer + *position, 8);
            value = rb_float_new(d);
            *position += 8;
            break;
        }
    case 2:
    case 13:
        {
            int value_length;
            value_length = *(int*)(buffer + *position) - 1;
            *position += 4;
            value = STR_NEW(buffer + *position, value_length);
            *position += value_length + 1;
            break;
        }
    case 3:
        {
            int size;
            memcpy(&size, buffer + *position, 4);
            if (strcmp(buffer + *position + 5, "$ref") == 0) { // DBRef
                VALUE argv[2];
                unsigned char id_type;
                int offset = *position + 10;
                int collection_length = *(int*)(buffer + offset) - 1;

                offset += 4;

                argv[0] = STR_NEW(buffer + offset, collection_length);
                offset += collection_length + 1;
                id_type = (unsigned char)buffer[offset];
                offset += 5;
                argv[1] = get_value(buffer, &offset, id_type, opts);
                value = rb_class_new_instance(2, argv, DBRef);
            } else {
                value = elements_to_hash(buffer + *position + 4, size - 5, opts);
            }
            *position += size;
            break;
        }
    case 4:
        {
            int size, end;
            memcpy(&size, buffer + *position, 4);
            end = *position + size - 1;
            *position += 4;

            value = rb_ary_new();
            while (*position < end) {
                VALUE to_append;
                unsigned char type = (unsigned char)buffer[(*position)++];
                int key_size = (int)strlen(buffer + *position);

                *position += key_size + 1; // just skip the key, they're in order.
                to_append = get_value(buffer, position, type, opts);
                rb_ary_push(value, to_append);
            }
            (*position)++;
            break;
        }
    case 5:
        {
            int length, subtype;
            VALUE data, st;
            VALUE argv[2];
            memcpy(&length, buffer + *position, 4);
            subtype = (unsigned char)buffer[*position + 4];
            if (subtype == 2) {
                data = rb_str_new(buffer + *position + 9, length - 4);
            } else {
                data = rb_str_new(buffer + *position + 5, length);
            }
            st = INT2FIX(subtype);
            argv[0] = data;
            argv[1] = st;
            value = rb_class_new_instance(2, argv, Binary);
            *position += length + 5;
            break;
        }
    case 6:
        {
            value = Qnil;
            break;
        }
    case 7:
        {
            VALUE str = rb_str_new(buffer + *position, 12);
            VALUE oid = rb_funcall(str, unpack_method, 1, rb_str_new2("C*"));
            value = rb_class_new_instance(1, &oid, ObjectId);
            *position += 12;
            break;
        }
    case 8:
        {
            value = buffer[(*position)++] ? Qtrue : Qfalse;
            break;
        }
    case 9:
        {
            int64_t millis;
            memcpy(&millis, buffer + *position, 8);

            // Support 64-bit time values in 32 bit environments in Ruby > 1.9
            // Note: rb_time_num_new is not available pre Ruby 1.9
            #if RUBY_API_VERSION_CODE >= 10900
                #define add(x,y) (rb_funcall((x), '+', 1, (y)))
                #define mul(x,y) (rb_funcall((x), '*', 1, (y)))
                #define quo(x,y) (rb_funcall((x), rb_intern("quo"), 1, (y)))
                VALUE d, timev;
                d = LL2NUM(1000LL);
                timev = add(LL2NUM(millis / 1000), quo(LL2NUM(millis % 1000), d));
                value = rb_time_num_new(timev, Qnil);
            #else
                value = rb_time_new(millis / 1000, (millis % 1000) * 1000);
            #endif

            value = rb_funcall(value, utc_method, 0);
            *position += 8;
            break;
        }
    case 10:
        {
            value = Qnil;
            break;
        }
    case 11:
        {
            int pattern_length = (int)strlen(buffer + *position);
            VALUE pattern = STR_NEW(buffer + *position, pattern_length);
            int flags_length;
            VALUE argv[3], flags_str;
            *position += pattern_length + 1;

            flags_length = (int)strlen(buffer + *position);
            flags_str = STR_NEW(buffer + *position, flags_length);
            argv[0] = pattern;
            argv[1] = flags_str;
            value = rb_class_new_instance(2, argv, BSONRegex);

            if (opts->compile_regex == 1) {
                value = rb_funcall(value, rb_intern("try_compile"), 0);
            }
            *position += flags_length + 1;
            break;
        }
    case 12:
        {
            int collection_length;
            VALUE collection, str, oid, id, argv[2];
            collection_length = *(int*)(buffer + *position) - 1;
            *position += 4;
            collection = STR_NEW(buffer + *position, collection_length);
            *position += collection_length + 1;

            str = rb_str_new(buffer + *position, 12);
            oid = rb_funcall(str, unpack_method, 1, rb_str_new2("C*"));
            id = rb_class_new_instance(1, &oid, ObjectId);
            *position += 12;

            argv[0] = collection;
            argv[1] = id;
            value = rb_class_new_instance(2, argv, DBRef);
            break;
        }
    case 14:
        {
            int value_length;
            memcpy(&value_length, buffer + *position, 4);
            value = ID2SYM(rb_intern(buffer + *position + 4));
            *position += value_length + 4;
            break;
        }
    case 15:
        {
            int code_length, scope_size;
            VALUE code, scope, argv[2];
            *position += 4;
            code_length = *(int*)(buffer + *position) - 1;
            *position += 4;
            code = STR_NEW(buffer + *position, code_length);
            *position += code_length + 1;

            memcpy(&scope_size, buffer + *position, 4);
            scope = elements_to_hash(buffer + *position + 4, scope_size - 5, opts);
            *position += scope_size;

            argv[0] = code;
            argv[1] = scope;
            value = rb_class_new_instance(2, argv, Code);
            break;
        }
    case 16:
        {
            int i;
            memcpy(&i, buffer + *position, 4);
            value = LL2NUM(i);
            *position += 4;
            break;
        }
    case 17:
        {
            unsigned int sec, inc;
            VALUE argv[2];
            memcpy(&inc, buffer + *position, 4);
            memcpy(&sec, buffer + *position + 4, 4);
            argv[0] = UINT2NUM(sec);
            argv[1] = UINT2NUM(inc);
            value = rb_class_new_instance(2, argv, Timestamp);
            *position += 8;
            break;
        }
    case 18:
        {
            long long ll;
            memcpy(&ll, buffer + *position, 8);
            value = LL2NUM(ll);
            *position += 8;
            break;
        }
    case 127:
        {
            value = rb_class_new_instance(0, NULL, MaxKey);
            break;
        }
    default:
        {
            rb_raise(rb_eTypeError, "Detected unknown BSON type \"\\x%d\". Are you using the latest BSON version?", type);
            break;
        }
    }
    return value;
}

static VALUE elements_to_hash(const char* buffer, int max, struct deserialize_opts * opts) {
    int position = 0;
    VALUE hash = rb_class_new_instance(0, NULL, OrderedHash);
    while (position < max) {
        VALUE value;
        unsigned char type = (unsigned char)buffer[position++];
        int name_length = (int)strlen(buffer + position);
        VALUE name = STR_NEW(buffer + position, name_length);
        position += name_length + 1;
        value = get_value(buffer, &position, type, opts);
        rb_funcall(hash, element_assignment_method, 2, name, value);
    }
    return hash;
}

static VALUE method_deserialize(VALUE self, VALUE bson, VALUE opts) {
    const char* buffer = RSTRING_PTR(bson);
    int remaining = RSTRING_LENINT(bson);
    struct deserialize_opts deserialize_opts;

    deserialize_opts.compile_regex = 1;
    if (rb_funcall(opts, rb_intern("has_key?"), 1, ID2SYM(rb_intern("compile_regex"))) == Qtrue &&
        rb_hash_aref(opts, ID2SYM(rb_intern("compile_regex"))) == Qfalse) {
        deserialize_opts.compile_regex = 0;
    }

    // NOTE we just swallow the size and end byte here
    buffer += 4;
    remaining -= 5;

    return elements_to_hash(buffer, remaining, &deserialize_opts);
}

static int legal_objectid_str(VALUE str) {
    int i;

    if (TYPE(str) != T_STRING) {
        return 0;
    }

    if (RSTRING_LEN(str) != 24) {
        return 0;
    }

    for(i = 0; i < 24; i++) {
        char c = RSTRING_PTR(str)[i];

        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))) {
            return 0;
        }
    }

    return 1;
}

static VALUE objectid_legal(VALUE self, VALUE str)
{
    if (legal_objectid_str(str))
        return Qtrue;
    return Qfalse;
}

static char hexbyte( char hex ) {
    if (hex >= '0' && hex <= '9')
        return (hex - '0');
    else if (hex >= 'A' && hex <= 'F')
        return (hex - 'A' + 10);
    else if (hex >= 'a' && hex <= 'f')
        return (hex - 'a' + 10);
    else
        return 0x0;
}

static VALUE objectid_from_string(VALUE self, VALUE str)
{
    VALUE oid;
    int i;

    if (!legal_objectid_str(str)) {
      if (TYPE(str) == T_STRING) {
        rb_raise(InvalidObjectId, "illegal ObjectId format: %s", RSTRING_PTR(str));
      } else {
        VALUE inspect;
        inspect = rb_funcall(str, rb_intern("to_s"), 0);
        rb_raise(InvalidObjectId, "not a String: %s", (char *)inspect);
      }
    }

    oid = rb_ary_new2(12);

    for(i = 0; i < 12; i++) {
        rb_ary_store(oid, i, INT2FIX( (unsigned)(hexbyte( RSTRING_PTR(str)[2*i] ) << 4 ) | hexbyte( RSTRING_PTR(str)[2*i + 1] )));
    }

    return rb_class_new_instance(1, &oid, ObjectId);
}

static VALUE objectid_to_s(VALUE self)
{
    VALUE data;
    char cstr[25];
    VALUE rstr;
    VALUE *data_arr;

    data = rb_iv_get(self, "@data");
    data_arr = RARRAY_PTR(data);

    sprintf(cstr, "%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
        (unsigned)NUM2INT(data_arr[0]), (unsigned)NUM2INT(data_arr[1]), (unsigned)NUM2INT(data_arr[2]), (unsigned)NUM2INT(data_arr[3]),
        (unsigned)NUM2INT(data_arr[4]), (unsigned)NUM2INT(data_arr[5]), (unsigned)NUM2INT(data_arr[6]), (unsigned)NUM2INT(data_arr[7]),
        (unsigned)NUM2INT(data_arr[8]), (unsigned)NUM2INT(data_arr[9]), (unsigned)NUM2INT(data_arr[10]), (unsigned)NUM2INT(data_arr[11]));

    rstr = rb_str_new(cstr, 24);

    return rstr;
}


static VALUE objectid_generate(int argc, VALUE* args, VALUE self)
{
    VALUE oid;
    unsigned char oid_bytes[12];
    unsigned long t, inc;
    unsigned short pid;
    int i;

    if(argc == 0 || (argc == 1 && *args == Qnil)) {
        t = htonl((int)time(NULL));
    } else {
        t = htonl(NUM2UINT(rb_funcall(*args, rb_intern("to_i"), 0)));
    }
    MEMCPY(&oid_bytes, &t, unsigned char, 4);

    MEMCPY(&oid_bytes[4], hostname_digest, unsigned char, 3);

    pid = htons(getpid());
    MEMCPY(&oid_bytes[7], &pid, unsigned char, 2);

    /* No need to synchronize modification of this counter between threads;
     * MRI global interpreter lock guarantees serializability.
     *
     * Compiler should optimize out impossible branch.
     */
    if (sizeof(unsigned int) == 4) {
        object_id_inc++;
    } else {
        object_id_inc = (object_id_inc + 1) % 0xFFFFFF;
    }
    inc = htonl(object_id_inc);
    MEMCPY(&oid_bytes[9], ((unsigned char*)&inc + 1), unsigned char, 3);

    oid = rb_ary_new2(12);
    for(i = 0; i < 12; i++) {
        rb_ary_store(oid, i, INT2FIX((unsigned int)oid_bytes[i]));
    }
    return oid;
}

static VALUE method_update_max_bson_size(VALUE self, VALUE connection) {
    max_bson_size = FIX2INT(rb_funcall(connection, rb_intern("max_bson_size"), 0));
    return INT2FIX(max_bson_size);
}

static VALUE method_max_bson_size(VALUE self) {
    return INT2FIX(max_bson_size);
}

void Init_cbson() {
    VALUE bson, CBson, Digest, ext_version, digest;
    static char hostname[MAX_HOSTNAME_LENGTH];

    element_assignment_method = rb_intern("[]=");
    unpack_method = rb_intern("unpack");
    utc_method = rb_intern("utc");
    lt_operator = rb_intern("<");
    gt_operator = rb_intern(">");

    bson = rb_const_get(rb_cObject, rb_intern("BSON"));
    rb_require("bson/types/binary");
    Binary = rb_const_get(bson, rb_intern("Binary"));
    rb_require("bson/types/object_id");
    ObjectId = rb_const_get(bson, rb_intern("ObjectId"));
    rb_require("bson/types/dbref");
    DBRef = rb_const_get(bson, rb_intern("DBRef"));
    rb_require("bson/types/code");
    Code = rb_const_get(bson, rb_intern("Code"));
    rb_require("bson/types/min_max_keys");
    MinKey = rb_const_get(bson, rb_intern("MinKey"));
    MaxKey = rb_const_get(bson, rb_intern("MaxKey"));
    rb_require("bson/types/timestamp");
    Timestamp = rb_const_get(bson, rb_intern("Timestamp"));
    rb_require("bson/types/regex");
    BSONRegex = rb_const_get(bson, rb_intern("Regex"));
    BSONRegex_IGNORECASE = FIX2INT(rb_const_get(BSONRegex, rb_intern("IGNORECASE")));
    BSONRegex_EXTENDED = FIX2INT(rb_const_get(BSONRegex, rb_intern("EXTENDED")));
    BSONRegex_MULTILINE = FIX2INT(rb_const_get(BSONRegex, rb_intern("MULTILINE")));
    BSONRegex_DOTALL = FIX2INT(rb_const_get(BSONRegex, rb_intern("DOTALL")));
    BSONRegex_LOCALE_DEPENDENT = FIX2INT(rb_const_get(BSONRegex, rb_intern("LOCALE_DEPENDENT")));
    BSONRegex_UNICODE = FIX2INT(rb_const_get(BSONRegex, rb_intern("UNICODE")));
    Regexp = rb_const_get(rb_cObject, rb_intern("Regexp"));
    rb_require("bson/exceptions");
    InvalidKeyName = rb_const_get(bson, rb_intern("InvalidKeyName"));
    InvalidStringEncoding = rb_const_get(bson, rb_intern("InvalidStringEncoding"));
    InvalidDocument = rb_const_get(bson, rb_intern("InvalidDocument"));
    InvalidObjectId = rb_const_get(bson, rb_intern("InvalidObjectId"));
    rb_require("bson/ordered_hash");
    OrderedHash = rb_const_get(bson, rb_intern("OrderedHash"));
    RB_HASH = rb_const_get(bson, rb_intern("Hash"));

    CBson = rb_define_module("CBson");
    ext_version = rb_str_new2(VERSION);
    rb_define_const(CBson, "VERSION", ext_version);
    rb_define_module_function(CBson, "serialize", method_serialize, 4);
    rb_define_module_function(CBson, "deserialize", method_deserialize, 2);
    rb_define_module_function(CBson, "max_bson_size", method_max_bson_size, 0);
    rb_define_module_function(CBson, "update_max_bson_size", method_update_max_bson_size, 1);

    rb_require("digest/md5");
    Digest = rb_const_get(rb_cObject, rb_intern("Digest"));
    DigestMD5 = rb_const_get(Digest, rb_intern("MD5"));

    rb_define_singleton_method(ObjectId, "legal?", objectid_legal, 1);
    rb_define_singleton_method(ObjectId, "from_string", objectid_from_string, 1);
    rb_define_method(ObjectId, "to_s", objectid_to_s, 0);
    rb_define_method(ObjectId, "generate", objectid_generate, -1);

    if (gethostname(hostname, MAX_HOSTNAME_LENGTH) != 0) {
        rb_raise(rb_eRuntimeError, "failed to get hostname");
    }
    digest = rb_funcall(DigestMD5, rb_intern("digest"), 1,
        rb_str_new2(hostname));
    memcpy(hostname_digest, RSTRING_PTR(digest), 16);
    hostname_digest[16] = '\0';

    max_bson_size = 4 * 1024 * 1024;
}
