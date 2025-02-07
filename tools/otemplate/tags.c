/**
  Onion HTTP server library
  Copyright (C) 2010-2018 David Moreno Montero and others

  This library is free software; you can redistribute it and/or
  modify it under the terms of, at your choice:

  a. the Apache License Version 2.0.

  b. the GNU General Public License as published by the
  Free Software Foundation; either version 2.0 of the License,
  or (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of both licenses, if not see
  <http://www.gnu.org/licenses/> and
  <http://www.apache.org/licenses/LICENSE-2.0>.
*/

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <libgen.h>
#include <assert.h>

#include <onion/log.h>
#include <onion/block.h>
#include <onion/codecs.h>
#include <onion/dict.h>

#include "variables.h"
#include "list.h"
#include "parser.h"
#include "tags.h"

void unquote_inplace(char *str);

/**
 * @short Creates a new token
 */
tag_token *tag_token_new(const char *data, int l, token_type t, char unquote) {
  tag_token *ret = malloc(sizeof(tag_token));
  ret->data = strndup(data, l);
  ret->type = t;
  if (unquote ) unquote_inplace(ret->data);
  return ret;
}

tag_token *tag_token_unquoted_new(const char *data, int l, token_type t) {
  tag_token *ret = malloc(sizeof(tag_token));
  ret->data = strndup(data, l);
  ret->type = t;
  return ret;
}

/**
 * @short Frees it
 */
void tag_token_free(tag_token * t) {
  free(t->data);
  free(t);
}

/// At tag_builtins.c, initializes the builtins
void tag_init_builtins();

/// Global list of known tags.
onion_dict *tags = NULL;

/**
 * @short Adds a tag to the tag dict
 * 
 * The f is the function to be called when this tag is found in the
 * code. f signature is:
 * 
 * void f(parser_status *st, list *l);
 * 
 * Check parser_status_t and some tag_example from tag_builtins.c to
 * know how to prepare one.
 */
void tag_add(const char *tagname, void *f) {
  ONION_DEBUG("Added tag %s", tagname);
  onion_dict_add(tags, tagname, f, OD_DUP_KEY);
}

/**
 * Current block is a tag, slice it and call the proper handler.
 */
void tag_write(parser_status * st, onion_block * b) {
  //ONION_DEBUG("Write tag %s",b->data);

  list *command = list_new((void *)tag_token_free);

  char mode = 0;                // 0 skip spaces, 1 in single var, 2 in quotes
  char backslash = 0;           // for mode=2: 0 no escaping, 1 escaping
  char unquote = 0;              // 1 if backslash was triggered once in token

  // Split into elements for the list
  int i, li = 0;
  const char *data = onion_block_data(b);
  int size = onion_block_size(b);
  for (i = 0; i < size; i++) {
    char c = data[i];
    switch (mode) {
    case 0:
      if (!isspace(c)) {
        if (c == '"') {
          mode = 2;
          backslash = 0;
          unquote = 0;
          li = i + 1;
        } else {
          mode = 1;
          li = i;
        }
      }
      break;
    case 1:
      if (isspace(c)) {
        mode = 0;
        list_add(command, tag_token_new(&data[li], i - li, T_VAR, 0));
      }
      break;
    case 2:
      if (c == '"'){
          if (backslash)
              unquote = 1;
          else{
              mode = 0;
              list_add(command, tag_token_new(&data[li], i - li, T_STRING, unquote));
          }
      }
      if (c == '\\')
          backslash = 1-backslash;
      else
          backslash = 0;
      break;
    }
  }
  if (mode == 1)
    list_add(command, tag_token_new(&data[li], i - li, T_VAR, 0));

  if (!command->head) {
    ONION_ERROR("%s:%d Incomplete command", st->infilename, st->line);
    st->status = 1;
    list_free(command);
    return;
  }
  // Call the function.
  tag_token *commandtoken = command->head->data;
  const char *commandname = commandtoken->data;

  void (*f) (parser_status * st, list * args);
  f = (void *)onion_dict_get(tags, commandname);
  if (f)
    f(st, command);
  else {
    ONION_ERROR("%s:%d Unknown command '%s'. ", st->infilename, st->line,
                commandname);
    st->status = 1;
  }

  list_free(command);
}

/// Returns the nth arg of the tag
const char *tag_value_arg(list * l, int n) {
  tag_token *t = list_get_n(l, n);
  if (t)
    return t->data;
  return NULL;
}

/// Returns the nth type of the tag
int tag_type_arg(list * l, int n) {
  tag_token *t = list_get_n(l, n);
  if (t)
    return t->type;
  return STRING;
}

/**
 * @short Frees tag related data. 
 * 
 * Must call tag_init later if neccesary.
 */
void tag_free() {
  if (tags)
    onion_dict_free(tags);
}

/**
 * @short Inits the tag dict
 * 
 * It calls the init_tag_builtins to fill the builtins tags.
 */
void tag_init() {
  tag_free();
  tags = onion_dict_new();
  tag_init_builtins();
}

/**
 * @short Replaces \" in tag token by ".
 */
void unquote_inplace(char *str) {
 /*
  * Attention, algorithm assumes that quote char '"' never occurs
  * without '\' prefix. This assumption holds for tag_write().
 */
    char *pos_in, *pos_out;
    pos_in = str;
    while (*pos_in++ != '\0' && *pos_in != '"'){ // str[_], str[_+1]
    }
    if(*pos_in == '\0') return; // no quote char contained.

    assert(pos_in > str);
    pos_out=pos_in;
    do {
        if( *pos_in == '"' ){
            assert(*(pos_in-1) == '\\');
            pos_out--;
        }
        *pos_out++ = *pos_in++; // str[_-d] = str[_]
    }while (*pos_in != '\0');
    *pos_out = '\0';
}
