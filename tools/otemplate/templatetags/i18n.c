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

#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <onion/codecs.h>
#include <onion/log.h>

#include "../tags.h"
#include "../functions.h"

/// Following text is for gettext
void tag_trans(parser_status * st, list * l) {
  char *s = onion_c_quote_new(tag_value_arg(l, 1));

  int sprintf_args = 0;
  const char *t = tag_value_arg(l, sprintf_args+2);
  while(t != NULL) {
      sprintf_args += 1;
      t = tag_value_arg(l, sprintf_args+2);
  }
  if (sprintf_args == 0){
      function_add_code(st,
              "  onion_response_write0(res, dgettext(onion_dict_get(context, \"LANG\"), %s));\n",
              s);
  }else{
      char tmpname[32]; // arg_%d
      char s_args[512]; // , arg1 [, arg2 […]]
      char *pos = s_args; // start position for snprintf
      int bytes_left = sizeof(s_args); // available space after pos
      int i;
      char *quoted_arg = NULL;

      function_add_code(st, "  {\n");
      for (i=0; i<sprintf_args; ++i){
          const char *arg = tag_value_arg(l, i+2);
          snprintf(tmpname, sizeof(tmpname), "arg_%d", i);

          assert(bytes_left >= 0);
          int bytes_written = snprintf(pos, bytes_left, ", %s", tmpname);
          if (bytes_written >= bytes_left){
              ONION_ERROR("trans argument list too long.");
              goto abort;
          }
          bytes_left -= bytes_written;
          pos += bytes_written;

          if (arg[0] == '{'){ // Lockback into dict at runtime
              int len = strlen(arg);
              if (arg[len-1] != '}'){
                  ONION_ERROR("trans argument not ends with '}'");
                  goto abort;
              }

              // '%.*s' construction to insert var for {var}.
              function_add_code(st,
                      "    const char *%s = onion_dict_get_dict(context, %.*s);\n",
                      tmpname, len-2, arg+1);
          }else{ // Instert string given by template.
              free(quoted_arg);
              quoted_arg = onion_c_quote_new(arg);

             function_add_code(st, "    const char *arg_%d = %s;", i, quoted_arg);
          }
      }

      function_add_code(st,
              "    char tmp[4096];\n"
              "    dgettext2(tmp, sizeof(tmp), onion_dict_get(context, \"LANG\"),\n"
              "        %s\n"      // Text with '%s's
              "        %s);\n"    // Args
              "    onion_response_write0(res, tmp);\n",
              s, s_args, s);
      function_add_code(st, "  }\n");

abort:
    free(quoted_arg);
  }
  free(s);
}

/// Adds the tags.
void plugin_init() {
  tag_add("trans", tag_trans);
}
