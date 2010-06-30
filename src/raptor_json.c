/* -*- Mode: c; c-basic-offset: 2 -*-
 *
 * raptor_json.c - Raptor JSON Parser
 *
 * JSON
 * http://n2.talis.com/wiki/RDF_JSON_Specification
 *
 * Copyright (C) 2001-2010, David Beckett http://www.dajobe.org/
 *
 * This package is Free Software and part of Redland http://librdf.org/
 *
 * It is licensed under the following three licenses as alternatives:
 *   1. GNU Lesser General Public License (LGPL) V2.1 or any newer version
 *   2. GNU General Public License (GPL) V2 or any newer version
 *   3. Apache License, V2.0 or any newer version
 *
 * You may not use this file except in compliance with at least one of
 * the above three licenses.
 *
 * See LICENSE.html or LICENSE.txt at the top of this package for the
 * complete terms and further detail along with the license texts for
 * the licenses in COPYING.LIB, COPYING and LICENSE-2.0.txt respectively.
 *
 *
 */


#ifdef HAVE_CONFIG_H
#include <raptor_config.h>
#endif

#ifdef WIN32
#include <win32_raptor_config.h>
#endif

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#ifdef HAVE_ERRNO_H
#include <errno.h>
#endif
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif

#include <yajl/yajl_parse.h>

/* Raptor includes */
#include "raptor.h"
#include "raptor_internal.h"

/* Set RAPTOR_DEBUG to > 1 to get lots of buffer related debugging */
/*
#undef RAPTOR_DEBUG
#define RAPTOR_DEBUG 2
*/

typedef enum {
  RAPTOR_JSON_STATE_ROOT,
  RAPTOR_JSON_STATE_MAP_ROOT,
  RAPTOR_JSON_STATE_TRIPLES_KEY,
  RAPTOR_JSON_STATE_TRIPLES_ARRAY,
  RAPTOR_JSON_STATE_TRIPLES_TRIPLE,
  RAPTOR_JSON_STATE_TRIPLES_TERM,
  RAPTOR_JSON_STATE_RESOURCES_SUBJECT_KEY,
  RAPTOR_JSON_STATE_RESOURCES_PREDICATE,
  RAPTOR_JSON_STATE_RESOURCES_OBJECT_ARRAY,
  RAPTOR_JSON_STATE_RESOURCES_OBJECT
} raptor_json_parse_state;

typedef enum {
  RAPTOR_JSON_TERM_UNKNOWN,
  RAPTOR_JSON_TERM_SUBJECT,
  RAPTOR_JSON_TERM_PREDICATE,
  RAPTOR_JSON_TERM_OBJECT
} raptor_json_term;

typedef enum {
  RAPTOR_JSON_ATTRIB_UNKNOWN,
  RAPTOR_JSON_ATTRIB_VALUE,
  RAPTOR_JSON_ATTRIB_LANG,
  RAPTOR_JSON_ATTRIB_TYPE,
  RAPTOR_JSON_ATTRIB_DATATYPE
} raptor_json_term_attrib;


/*
 * JSON parser object
 */
struct raptor_json_parser_context_s {
  yajl_parser_config config;
  yajl_handle handle;

  /* Parser state */
  raptor_json_parse_state state;
  raptor_json_term        term;
  raptor_json_term_attrib attrib;

  /* Temporary storage, while creating terms */
  raptor_term_type term_type;
  unsigned char*   term_value;
  unsigned char*   term_datatype;
  unsigned char*   term_lang;

  /* Temporary storage, while creating statements */
  raptor_statement statement;
};

typedef struct raptor_json_parser_context_s raptor_json_parser_context;


static void
raptor_json_reset_term(raptor_json_parser_context *context)
{
  if (context->term_value)    RAPTOR_FREE(cstring,context->term_value);
  if (context->term_lang)     RAPTOR_FREE(cstring,context->term_lang);
  if (context->term_datatype) RAPTOR_FREE(cstring,context->term_datatype);

  context->term_value = NULL;
  context->term_lang = NULL;
  context->term_datatype = NULL;
  context->term_type = RAPTOR_TERM_TYPE_UNKNOWN;
  context->attrib = RAPTOR_JSON_ATTRIB_UNKNOWN;
}

static unsigned char*
raptor_json_cstring_from_counted_string(raptor_parser *rdf_parser, const unsigned char* str, unsigned int len)
{
  unsigned char *cstr = (unsigned char*)RAPTOR_MALLOC(cstring, len+1);
  if (!cstr) {
    raptor_parser_fatal_error(rdf_parser, "Out of memory");
    return NULL;
  }

  memcpy(cstr, str, len);
  cstr[len] = '\0';

  return cstr;
}

static raptor_term*
raptor_json_new_term_from_counted_string(raptor_parser *rdf_parser, const unsigned char* str, unsigned int len)
{
  raptor_term *term = NULL;

  if (len > 2 && str[0] == '_' && str[1] == ':') {
    const unsigned char *node_id = &str[2];
    term = raptor_new_term_from_counted_blank(rdf_parser->world, node_id, len-2);

  } else {
    raptor_uri *uri = raptor_new_uri_from_counted_string(rdf_parser->world, str, len);
    if (!uri) {
      unsigned char* cstr = raptor_json_cstring_from_counted_string(rdf_parser,str,len);
      raptor_parser_error(rdf_parser, "Could not create uri from '%s'", cstr);
      RAPTOR_FREE(cstring,cstr);
      return NULL;
    }

    term = raptor_new_term_from_uri(rdf_parser->world, uri);
    raptor_free_uri(uri);
  }

  return term;
}


static raptor_term*
raptor_json_generate_term(raptor_parser *rdf_parser)
{
  raptor_json_parser_context *context = (raptor_json_parser_context*)rdf_parser->context;
  raptor_term *term = NULL;

  if (!context->term_value) {
    raptor_parser_error(rdf_parser, "No value for term defined.");
    return NULL;
  }

  switch(context->term_type) {
    case RAPTOR_TERM_TYPE_URI: {
      raptor_uri *uri = raptor_new_uri(rdf_parser->world, context->term_value);
      if(!uri) {
        raptor_parser_error(rdf_parser, "Could not create uri from '%s'", context->term_value);
        return NULL;
      }
      term = raptor_new_term_from_uri(rdf_parser->world, uri);
      raptor_free_uri(uri);
      break;
    }
    case RAPTOR_TERM_TYPE_LITERAL: {
      raptor_uri *datatype_uri = NULL;
      if (context->term_datatype) {
        datatype_uri = raptor_new_uri(rdf_parser->world, context->term_datatype);
      }
      term = raptor_new_term_from_literal(rdf_parser->world, context->term_value, datatype_uri, context->term_lang);
      if (datatype_uri) {
        raptor_free_uri(datatype_uri);
      }
      break;
    }
    case RAPTOR_TERM_TYPE_BLANK: {
      unsigned char *node_id = context->term_value;
      if (strlen((const char*)node_id) > 2 && node_id[0] == '_' && node_id[1] == ':') {
          node_id = &node_id[2];
      }
      term = raptor_new_term_from_blank(rdf_parser->world, node_id);
      break;
    }
    case RAPTOR_TERM_TYPE_UNKNOWN:
    default:
      raptor_parser_error(rdf_parser, "Unsupported term type in raptor_json_generate_term.");
      break;
  }

  return term;
}


static int raptor_json_yajl_null(void * ctx)
{
  raptor_parser* rdf_parser = (raptor_parser*)ctx;
  raptor_parser_error(rdf_parser,"Nulls are not valid in RDF/JSON");
  return 0;
}

static int raptor_json_yajl_boolean(void * ctx, int b)
{
  raptor_parser* rdf_parser = (raptor_parser*)ctx;
  raptor_parser_error(rdf_parser,"Booleans are not valid in RDF/JSON");
  return 0;
}

static int raptor_json_yajl_integer(void * ctx, long l)
{
  raptor_parser* rdf_parser = (raptor_parser*)ctx;
  raptor_parser_error(rdf_parser,"Integers are not valid in RDF/JSON");
  return 0;
}

static int raptor_json_yajl_double(void * ctx, double d)
{
  raptor_parser* rdf_parser = (raptor_parser*)ctx;
  raptor_parser_error(rdf_parser,"Floats are not valid in RDF/JSON");
  return 0;
}

static int raptor_json_yajl_string(void * ctx, const unsigned char * str,
                           unsigned int len)
{
  raptor_parser* rdf_parser = (raptor_parser*)ctx;
  raptor_json_parser_context *context;
  context = (raptor_json_parser_context*)rdf_parser->context;

  if (context->state == RAPTOR_JSON_STATE_TRIPLES_TERM ||
      context->state == RAPTOR_JSON_STATE_RESOURCES_OBJECT) {
    switch (context->attrib) {
      case RAPTOR_JSON_ATTRIB_VALUE:
        context->term_value = raptor_json_cstring_from_counted_string(rdf_parser,str,len);
      break;
      case RAPTOR_JSON_ATTRIB_LANG:
        context->term_lang = raptor_json_cstring_from_counted_string(rdf_parser,str,len);
      break;
      case RAPTOR_JSON_ATTRIB_TYPE:
        if (!strncmp((const char*)str,"uri",len)) {
          context->term_type = RAPTOR_TERM_TYPE_URI;
        } else if (!strncmp((const char*)str,"literal",len)) {
          context->term_type = RAPTOR_TERM_TYPE_LITERAL;
        } else if (!strncmp((const char*)str,"bnode",len)) {
          context->term_type = RAPTOR_TERM_TYPE_BLANK;
        } else {
          unsigned char * cstr = raptor_json_cstring_from_counted_string(rdf_parser,str,len);
          context->term_type = RAPTOR_TERM_TYPE_UNKNOWN;
          raptor_parser_error(rdf_parser,"Unknown term type: %s", cstr);
          RAPTOR_FREE(cstring,cstr);
        }
      break;
      case RAPTOR_JSON_ATTRIB_DATATYPE:
        context->term_datatype = raptor_json_cstring_from_counted_string(rdf_parser,str,len);
      break;
      case RAPTOR_JSON_ATTRIB_UNKNOWN:
      default:
        raptor_parser_error(rdf_parser,"Unsupported term attribute in raptor_json_string");
      break;
    }
  } else {
    raptor_parser_error(rdf_parser,"Unexpected JSON string");
    return 0;
  }
  return 1;
}

static int raptor_json_yajl_map_key(void * ctx, const unsigned char * str,
                            unsigned int len)
{
  raptor_parser* rdf_parser = (raptor_parser*)ctx;
  raptor_json_parser_context *context;
  context = (raptor_json_parser_context*)rdf_parser->context;

  if (context->state == RAPTOR_JSON_STATE_MAP_ROOT) {
    if (!strncmp((const char*)str,"triples",len)) {
      context->state = RAPTOR_JSON_STATE_TRIPLES_KEY;
      return 1;
    } else {
      if (context->statement.subject)
        raptor_free_term(context->statement.subject);
      context->statement.subject = raptor_json_new_term_from_counted_string(rdf_parser, str, len);
      if (!context->statement.subject)
        return 0;
      context->state = RAPTOR_JSON_STATE_RESOURCES_SUBJECT_KEY;
      return 1;
    }
  } else if (context->state == RAPTOR_JSON_STATE_RESOURCES_PREDICATE) {
    if (context->statement.predicate)
      raptor_free_term(context->statement.predicate);
    context->statement.predicate = raptor_json_new_term_from_counted_string(rdf_parser, str, len);
    if (!context->statement.predicate)
        return 0;
    return 1;
  } else if (context->state == RAPTOR_JSON_STATE_TRIPLES_TRIPLE) {
    if (!strncmp((const char*)str,"subject",len)) {
      context->term = RAPTOR_JSON_TERM_SUBJECT;
      return 1;
    } else if (!strncmp((const char*)str,"predicate",len)) {
      context->term = RAPTOR_JSON_TERM_PREDICATE;
      return 1;
    } else if (!strncmp((const char*)str,"object",len)) {
      context->term = RAPTOR_JSON_TERM_OBJECT;
      return 1;
    } else {
      raptor_parser_error(rdf_parser,"Unexpected JSON key name in triple definition.");
      return 0;
    }
  } else if (context->state == RAPTOR_JSON_STATE_TRIPLES_TERM ||
             context->state == RAPTOR_JSON_STATE_RESOURCES_OBJECT) {
    if (!strncmp((const char*)str,"value",len)) {
      context->attrib = RAPTOR_JSON_ATTRIB_VALUE;
      return 1;
    } else if (!strncmp((const char*)str,"type",len)) {
      context->attrib = RAPTOR_JSON_ATTRIB_TYPE;
      return 1;
    } else if (!strncmp((const char*)str,"datatype",len)) {
      context->attrib = RAPTOR_JSON_ATTRIB_DATATYPE;
      return 1;
    } else if (!strncmp((const char*)str,"lang",len)) {
      context->attrib = RAPTOR_JSON_ATTRIB_LANG;
      return 1;
    } else {
      context->attrib = RAPTOR_JSON_ATTRIB_UNKNOWN;
      raptor_parser_error(rdf_parser,"Unexpected key name in triple definition.");
      return 0;
    }
    return 1;
  } else {
    raptor_parser_error(rdf_parser,"Unexpected JSON map key.");
    return 0;
  }
}

static int raptor_json_yajl_start_map(void * ctx)
{
  raptor_parser* rdf_parser = (raptor_parser*)ctx;
  raptor_json_parser_context *context;
  context = (raptor_json_parser_context*)rdf_parser->context;

  if (context->state == RAPTOR_JSON_STATE_ROOT) {
    context->state = RAPTOR_JSON_STATE_MAP_ROOT;
    return 1;
  } else if (context->state == RAPTOR_JSON_STATE_RESOURCES_SUBJECT_KEY) {
    context->state = RAPTOR_JSON_STATE_RESOURCES_PREDICATE;
    return 1;
  } else if (context->state == RAPTOR_JSON_STATE_RESOURCES_OBJECT_ARRAY) {
    context->state = RAPTOR_JSON_STATE_RESOURCES_OBJECT;
    return 1;
  } else if (context->state == RAPTOR_JSON_STATE_TRIPLES_ARRAY) {
    raptor_statement_clear(&context->statement);
    context->term = RAPTOR_JSON_TERM_UNKNOWN;
    context->state = RAPTOR_JSON_STATE_TRIPLES_TRIPLE;
    return 1;
  } else if (context->state == RAPTOR_JSON_STATE_TRIPLES_TRIPLE) {
    context->state = RAPTOR_JSON_STATE_TRIPLES_TERM;
    raptor_json_reset_term(context);
    return 1;
  } else {
    raptor_parser_error(rdf_parser,"Unexpected start of JSON map.");
    return 0;
  }
}


static int raptor_json_yajl_end_map(void * ctx)
{
  raptor_parser* rdf_parser = (raptor_parser*)ctx;
  raptor_json_parser_context *context;
  context = (raptor_json_parser_context*)rdf_parser->context;

  if (context->state == RAPTOR_JSON_STATE_RESOURCES_OBJECT) {
    context->statement.object = raptor_json_generate_term(rdf_parser);
    if (!context->statement.object) return 0;

    /* Generate the statement */
    (*rdf_parser->statement_handler)(rdf_parser->user_data, &context->statement);

    raptor_free_term(context->statement.object);
    context->statement.object = NULL;
    raptor_json_reset_term(context);

    context->state = RAPTOR_JSON_STATE_RESOURCES_OBJECT_ARRAY;
    return 1;
  } else if (context->state == RAPTOR_JSON_STATE_RESOURCES_PREDICATE) {
    context->state = RAPTOR_JSON_STATE_MAP_ROOT;
    return 1;
  } else if (context->state == RAPTOR_JSON_STATE_TRIPLES_TERM) {
    raptor_term *term = raptor_json_generate_term(rdf_parser);
    if (!term) return 0;

    // Store the term in the statement
    switch(context->term) {
      case RAPTOR_JSON_TERM_SUBJECT:
        if (context->statement.subject)
          raptor_free_term(context->statement.subject);
        context->statement.subject = term;
      break;
      case RAPTOR_JSON_TERM_PREDICATE:
        if (context->statement.predicate)
          raptor_free_term(context->statement.predicate);
        context->statement.predicate = term;
      break;
      case RAPTOR_JSON_TERM_OBJECT:
        if (context->statement.object)
          raptor_free_term(context->statement.object);
        context->statement.object = term;
      break;
      case RAPTOR_JSON_TERM_UNKNOWN:
      default:
        raptor_parser_error(rdf_parser, "Unknown term in raptor_json_end_map");
      break;
    }

    context->state = RAPTOR_JSON_STATE_TRIPLES_TRIPLE;
    raptor_json_reset_term(context);
    return 1;
  } else if (context->state == RAPTOR_JSON_STATE_TRIPLES_TRIPLE) {
    if (!context->statement.subject) {
      raptor_parser_error(rdf_parser, "Triple is missing a subject term");
      return 0;
    } else if (!context->statement.predicate) {
      raptor_parser_error(rdf_parser, "Triple is missing a predicate term");
      return 0;
    } else if (!context->statement.object) {
      raptor_parser_error(rdf_parser, "Triple is missing a object term");
      return 0;
    } else {
      /* Generate the statement */
      (*rdf_parser->statement_handler)(rdf_parser->user_data, &context->statement);
    }
    raptor_statement_clear(&context->statement);
    context->state = RAPTOR_JSON_STATE_TRIPLES_ARRAY;
    return 1;
  } else if (context->state == RAPTOR_JSON_STATE_TRIPLES_KEY) {
    context->state = RAPTOR_JSON_STATE_MAP_ROOT;
    return 1;
  } else if (context->state == RAPTOR_JSON_STATE_MAP_ROOT) {
    context->state = RAPTOR_JSON_STATE_ROOT;
    return 1;
  } else {
    raptor_parser_error(rdf_parser,"Unexpected end of JSON map");
    return 0;
  }
}

static int raptor_json_yajl_start_array(void * ctx)
{
  raptor_parser* rdf_parser = (raptor_parser*)ctx;
  raptor_json_parser_context *context;
  context = (raptor_json_parser_context*)rdf_parser->context;

  if (context->state == RAPTOR_JSON_STATE_RESOURCES_PREDICATE) {
    context->state = RAPTOR_JSON_STATE_RESOURCES_OBJECT_ARRAY;
    return 1;
  } else if (context->state == RAPTOR_JSON_STATE_TRIPLES_KEY) {
    context->state = RAPTOR_JSON_STATE_TRIPLES_ARRAY;
    return 1;
  } else {
    raptor_parser_error(rdf_parser,"Unexpected start of array");
    return 0;
  }
}

static int raptor_json_yajl_end_array(void * ctx)
{
  raptor_parser* rdf_parser = (raptor_parser*)ctx;
  raptor_json_parser_context *context;
  context = (raptor_json_parser_context*)rdf_parser->context;

  if (context->state == RAPTOR_JSON_STATE_RESOURCES_OBJECT_ARRAY) {
    context->state = RAPTOR_JSON_STATE_RESOURCES_PREDICATE;
    return 1;
  } else if (context->state == RAPTOR_JSON_STATE_TRIPLES_ARRAY) {
    context->state = RAPTOR_JSON_STATE_MAP_ROOT;
    return 1;
  } else {
    raptor_parser_error(rdf_parser,"Unexpected end of array");
    return 0;
  }
}


static void*
raptor_json_yajl_malloc(void *ctx, unsigned int sz)
{
  return RAPTOR_MALLOC(cstring, sz);
}

static void*
raptor_json_yajl_realloc(void *ctx, void * ptr, unsigned int sz)
{
  return RAPTOR_REALLOC(cstring, ptr, sz);
}

static void
raptor_json_yajl_free(void *ctx, void * ptr)
{
  RAPTOR_FREE(cstring, ptr);
}

static yajl_alloc_funcs raptor_json_yajl_alloc_funcs = {
  raptor_json_yajl_malloc,
  raptor_json_yajl_realloc,
  raptor_json_yajl_free,
  NULL
};

static yajl_callbacks raptor_json_yajl_callbacks = {
  raptor_json_yajl_null,
  raptor_json_yajl_boolean,
  raptor_json_yajl_integer,
  raptor_json_yajl_double,
  NULL,
  raptor_json_yajl_string,
  raptor_json_yajl_start_map,
  raptor_json_yajl_map_key,
  raptor_json_yajl_end_map,
  raptor_json_yajl_start_array,
  raptor_json_yajl_end_array
};



/**
 * raptor_json_parse_init:
 *
 * Initialise the Raptor JSON parser.
 *
 * Return value: non 0 on failure
 **/

static int
raptor_json_parse_init(raptor_parser* rdf_parser, const char *name)
{
  raptor_json_parser_context *context;
  context = (raptor_json_parser_context*)rdf_parser->context;

  /* Initialse the static statement */
  raptor_statement_init(&context->statement, rdf_parser->world);

  /* Configure the parser */
  context->config.allowComments = 1;
  context->config.checkUTF8 = 0;

  return 0;
}


/*
 * raptor_json_parse_terminate - Free the Raptor JSON parser
 * @rdf_parser: parser object
 *
 **/
static void
raptor_json_parse_terminate(raptor_parser* rdf_parser)
{
  raptor_json_parser_context *context;
  context = (raptor_json_parser_context*)rdf_parser->context;

  if (context->handle) {
    yajl_free(context->handle);
  }

  raptor_json_reset_term(context);
  raptor_statement_clear(&context->statement);
}



static int
raptor_json_parse_chunk(raptor_parser* rdf_parser,
                            const unsigned char *s, size_t len,
                            int is_end)
{
  raptor_json_parser_context *context = (raptor_json_parser_context*)rdf_parser->context;
  int result = 0;
  yajl_status status;

#if defined(RAPTOR_DEBUG) && RAPTOR_DEBUG > 1
  RAPTOR_DEBUG2("adding %d bytes to buffer\n", (unsigned int)len);
#endif

  if (len) {
    /* Parse the chunk passed to us */
    status = yajl_parse(context->handle, s, len);

    if (status != yajl_status_ok &&
        status != yajl_status_insufficient_data)
    {
      unsigned char * str = yajl_get_error(context->handle, 1, s, len);
      raptor_parser_error(rdf_parser, "YAJL error: %s", (const char *) str);
      result = 1;
      yajl_free_error(context->handle, str);
    }
  }

  if (is_end) {
    /* parse any remaining buffered data */
    status = yajl_parse_complete(context->handle);

    if (status != yajl_status_ok)
    {
      unsigned char * str = yajl_get_error(context->handle, 0, NULL, 0);
      raptor_parser_error(rdf_parser, "YAJL error: %s", (const char *) str);
      result = 1;
      yajl_free_error(context->handle, str);
    }

    raptor_json_reset_term(context);
    raptor_statement_clear(&context->statement);
  }

  return result;
}


static int
raptor_json_parse_start(raptor_parser* rdf_parser)
{
  raptor_json_parser_context *context = (raptor_json_parser_context*)rdf_parser->context;

  /* Destroy the old parser */
  if (context->handle)
    yajl_free(context->handle);

  /* Initialise a new parser */
  context->handle = yajl_alloc(
    &raptor_json_yajl_callbacks,
    &context->config,
    &raptor_json_yajl_alloc_funcs,
    (void *)rdf_parser
  );

  if (!context->handle) {
    raptor_parser_fatal_error(rdf_parser, "Failed to initialise YAJL parser");
    return 1;
  }

  /* Initialise the parse state */
  context->state = RAPTOR_JSON_STATE_ROOT;
  context->state = RAPTOR_JSON_TERM_UNKNOWN;
  raptor_json_reset_term(context);
  raptor_statement_clear(&context->statement);

  return 0;
}


static int
raptor_json_parse_recognise_syntax(raptor_parser_factory* factory,
                                       const unsigned char *buffer, size_t len,
                                       const unsigned char *identifier,
                                       const unsigned char *suffix,
                                       const char *mime_type)
{
  unsigned int pos = 0;
  int score = 0;

  if (suffix) {
    if (!strcmp((const char*)suffix, "json"))
      score = 8;
    if (!strcmp((const char*)suffix, "js"))
      score = 3;
  } else if (identifier) {
    if (strstr((const char*)identifier, "json"))
      score = 4;
  }

  if (mime_type) {
    if (strstr((const char*)mime_type, "json"))
      score += 6;
  }

  // Is the first non-whitespace character a curly brace?
  while (pos<len) {
    if (isspace(buffer[pos])) {
      pos++;
    } else {
      if (buffer[pos] == '{')
        score += 2;
      break;
    }
  }

  return score;
}


static const char* const json_names[2] = { "json", NULL };

#define JSON_TYPES_COUNT 2
static const raptor_type_q json_types[JSON_TYPES_COUNT + 1] = {
  { "application/json", 16, 1},
  { "text/json", 9, 1},
  { NULL, 0, 0}
};

static int
raptor_json_parser_register_factory(raptor_parser_factory *factory)
{
  int rc = 0;

  factory->desc.names = json_names;

  factory->desc.mime_types = json_types;
  factory->desc.mime_types_count = JSON_TYPES_COUNT;

  factory->desc.label = "JSON";
  factory->desc.uri_string = NULL; // "http://n2.talis.com/wiki/RDF_JSON_Specification";

  factory->desc.flags = 0;

  factory->context_length     = sizeof(raptor_json_parser_context);

  factory->init      = raptor_json_parse_init;
  factory->terminate = raptor_json_parse_terminate;
  factory->start     = raptor_json_parse_start;
  factory->chunk     = raptor_json_parse_chunk;
  factory->recognise_syntax = raptor_json_parse_recognise_syntax;

  return rc;
}


int
raptor_init_parser_json(raptor_world* world)
{
  return !raptor_world_register_parser_factory(world,
                                               &raptor_json_parser_register_factory);
}