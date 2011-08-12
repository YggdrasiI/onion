/*
	Onion HTTP server library
	Copyright (C) 2010-2011 David Moreno Montero

	This library is free software; you can redistribute it and/or
	modify it under the terms of the GNU Lesser General Public
	License as published by the Free Software Foundation; either
	version 3.0 of the License, or (at your option) any later version.

	This library is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
	Lesser General Public License for more details.

	You should have received a copy of the GNU Lesser General Public
	License along with this library; if not see <http://www.gnu.org/licenses/>.
	*/
#define USE_SENDFILE

#include <string.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <errno.h>
#ifdef USE_SENDFILE
#include <sys/sendfile.h>
#endif

#include "onion.h"
#include "log.h"
#include "shortcuts.h"
#include "dict.h"
#include "block.h"
#include "mime.h"
#include "types_internal.h"

#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0
#endif

int onion_write_to_socket(int *fd, const char *data, unsigned int len);

/**
 * @short Shortcut for fast responses, like errors.
 * 
 * Prepares a fast response. You pass only the request, the text and the code, and it do the full response
 * object and sends the data.
 */
int onion_shortcut_response(const char* response, int code, onion_request* req, onion_response *res)
{
	return onion_shortcut_response_extra_headers(response, code, req, res, NULL);
}


/**
 * @short Shortcut for fast responses, like errors, with extra headers
 * 
 * Prepares a fast response. You pass only the request, the text and the code, and it do the full response
 * object and sends the data.
 * 
 * On this version you also pass a NULL terminated list of headers, in key, value pairs.
 */
int onion_shortcut_response_extra_headers(const char* response, int code, onion_request* req, onion_response *res, ... ){
	unsigned int l=strlen(response);
	const char *key, *value;
	
	onion_response_set_length(res,l);
	onion_response_set_code(res,code);
	
	va_list ap;
	va_start(ap, res);
	while ( (key=va_arg(ap, const char *)) ){
		value=va_arg(ap, const char *);
		if (key && value)
			onion_response_set_header(res, key, value);
		else
			break;
	}
	va_end(ap);

	onion_response_write_headers(res);
	
	onion_response_write(res,response,l);
	return OCS_PROCESSED;
}


/**
 * @short Shortcut to ease a redirect. 
 * 
 * It can be used directly as a handler, or be called from a handler.
 * 
 * The browser message is fixed; if need more flexibility, create your own redirector.
 */
int onion_shortcut_redirect(const char *newurl, onion_request *req, onion_response *res){
	return onion_shortcut_response_extra_headers("<h1>302 - Moved</h1>", HTTP_REDIRECT, req, res,
																							 "Location", newurl, NULL );
}

/**
 * @short This shortcut returns the given file contents. 
 * 
 * It sets all the compilant headers (TODO), cache and so on.
 * 
 * This is the recomended way to send static files; it even can use sendfile Linux call 
 * if suitable (TODO).
 * 
 * It does no security checks, so caller must be security aware.
 */
int onion_shortcut_response_file(const char *filename, onion_request *request, onion_response *res){
	int fd=open(filename,O_RDONLY|O_CLOEXEC);
	
	if (fd<0)
		return OCS_NOT_PROCESSED;

	if(SOCK_CLOEXEC == 0) { // Good compiler know how to cut this out
		int flags=fcntl(fd, F_GETFD);
		if (flags==-1){
			ONION_ERROR("Retrieving flags from file descriptor");
		}
		flags|=FD_CLOEXEC;
		if (fcntl(fd, F_SETFD, flags)==-1){
			ONION_ERROR("Setting O_CLOEXEC to file descriptor");
		}
	}
	
	struct stat st;
	if (stat(filename, &st)!=0){
		ONION_WARNING("File does not exist: %s",filename);
		close(fd);
		return OCS_NOT_PROCESSED;
	}
	
	if (S_ISDIR(st.st_mode)){
		close(fd);
		return OCS_NOT_PROCESSED;
	}
	
	size_t length=st.st_size;
	
	char etag[32];
	onion_shortcut_etag(&st, etag);
		
	ONION_DEBUG0("Etag %s", etag);
	const char *prev_etag=onion_request_get_header(request, "If-None-Match");
	if (prev_etag && (strcmp(prev_etag, etag)==0)){
		ONION_DEBUG0("Not modified");
		onion_response_set_length(res, 0);
		onion_response_set_code(res, HTTP_NOT_MODIFIED);
		onion_response_write_headers(res);
		close(fd);
		return OCS_PROCESSED;
	}
	
	onion_response_set_header(res, "Etag", etag);
	
	const char *range=onion_request_get_header(request, "Range");
	if (range && strncmp(range,"bytes=",6)==0){
		//ONION_DEBUG("Need just a range: %s",range);
		char tmp[1024];
		strncpy(tmp, range+6, 1024);
		char *start=tmp;
		char *end=tmp;
		while (*end!='-' && *end) end++;
		if (*end=='-'){
			*end='\0';
			end++;
			
			//ONION_DEBUG("Start %s, end %s",start,end);
			size_t ends, starts;
			if (*end)
				ends=atol(end);
			else
				ends=length;
			starts=atol(start);
			length=ends-starts;
			lseek(fd, starts, SEEK_SET);
			snprintf(tmp,sizeof(tmp),"%d-%d/%d",(unsigned int)starts, (unsigned int)ends-1, (unsigned int)length);
			onion_response_set_header(res, "Content-Range",tmp);
		}
	}
	
	onion_response_set_length(res, length);
	onion_response_set_header(res, "Content-Type", onion_mime_get(filename) );
	ONION_DEBUG("Mime type is %s",onion_mime_get(filename));
	onion_response_write_headers(res);
	
	if ((onion_request_get_flags(request)&OR_HEAD) == OR_HEAD){ // Just head.
		length=0;
	}
	
	if (length){
#ifdef USE_SENDFILE
		if (request->server->write==(void*)onion_write_to_socket){ // Lets have a house party! I can use sendfile!
			onion_response_write(res,NULL,0);
			ONION_DEBUG("Using sendfile");
			int r=sendfile(*((int*)request->socket), fd, NULL, length);
			ONION_DEBUG("Wrote %d, should be %d", r, length);
			if (r!=length || r<0){
				ONION_ERROR("Could not send all file (%s)", strerror(errno));
				close(fd);
				return OCS_INTERNAL_ERROR;
			}
			res->sent_bytes+=length;
			res->sent_bytes_total+=length;
		}
		else
#endif
		{ // Ok, no I cant, do it as always.
			int r=0,w;
			size_t tr=0;
			char tmp[4046];
			if (length>sizeof(tmp)){
				size_t max=length-sizeof(tmp);
				while( tr<max ){
					r=read(fd,tmp,sizeof(tmp));
					tr+=r;
					if (r<0)
						break;
					w=onion_response_write(res, tmp, r);
					if (w!=r){
						ONION_ERROR("Wrote less than read: write %d, read %d. Quite probably closed connection.",w,r);
						break;
					}
				}
			}
			if (sizeof(tmp) >= (length-tr)){
				r=read(fd, tmp, length-tr);
				w=onion_response_write(res, tmp, r);
				if (w!=r){
					ONION_ERROR("Wrote less than read: write %d, read %d. Quite probably closed connection.",w,r);
				}
			}
		}
	}
	close(fd);
	return OCS_PROCESSED;
}

/**
 * @short Shortcut to answer some json data
 * 
 * It converts to json the passed dict and returns it. The dict is freed before returning.
 */
int onion_shortcut_response_json(onion_dict *d, onion_request *req, onion_response *res){
	onion_response_set_header(res, "Content-Type", "application/json");
	
	onion_block *bl=onion_dict_to_json(d);
	onion_dict_free(d);
	char tmp[16];
	snprintf(tmp,sizeof(tmp),"%ld",(long)onion_block_size(bl));
	int ret=onion_shortcut_response_extra_headers(onion_block_data(bl), HTTP_OK, req, res, "Content-Length", tmp, NULL);
	onion_block_free(bl);
	return ret;
}

/**
 * @short Transforms a time_t to a RFC 822 date string
 * 
 * This date format is the standard in HTTP protocol as RFC 2616, section 3.3.1
 * 
 * The dest pointer must be at least 32 bytes long as thats the maximum size of the date.
 */
void onion_shortcut_date_string(time_t t, char *dest){
	struct tm ts;
	gmtime_r(&t, &ts);
	strftime(dest, t, "%a, %d %b %Y %T %Z", &ts);
}


/**
 * @short Transforms a time_t to a ISO date string
 * 
 * The dest pointer must be at least 21 bytes long as thats the maximum size of the date.
 */
void onion_shortcut_date_string_iso(time_t t, char *dest){
	struct tm ts;
	gmtime_r(&t, &ts);
	strftime(dest, t, "%FT%TZ", &ts);
}

/**
 * @short Unifies the creation of etags.
 * 
 * Just now its a very simple one, based on the size and date.
 */
void onion_shortcut_etag(struct stat *st, char etag[32]){
	size_t size=st->st_size;
	unsigned int time=st->st_mtime;
	snprintf(etag,32,"%04X-%04X",size,time);
	ONION_DEBUG0("Etag is %s", etag);
}

/**
 * @short Moves a file to another location
 * 
 * It takes care if it can be a simple rename or must copy and remove old.
 */
int onion_shortcut_rename(const char *orig, const char *dest){
	int ok=rename(orig, dest);
	
	if (ok!=0 && errno==EXDEV){ // Ok, old way, open both, copy
		ONION_DEBUG0("Slow cp, as tmp is in another FS");
		ok=0;
		int fd_dest=open(dest, O_WRONLY|O_CREAT, 0666);
		if (fd_dest<0){
			ok=1;
			ONION_ERROR("Could not open destination for writing (%s)", strerror(errno));
		}
		int fd_orig=open(orig, O_RDONLY);
		if (fd_dest<0){
			ok=1;
			ONION_ERROR("Could not open orig for reading (%s)", strerror(errno));
		}
		if (ok==0){
			char tmp[4096];
			int r;
			while ( (r=read(fd_orig, tmp, sizeof(tmp))) > 0 ){
				r=write(fd_dest, tmp, r);
				if (r<0){
					ONION_ERROR("Error writing to destination file (%s)", strerror(errno));
					ok=1;
					break;
				}
			}
		}
		if (fd_orig>=0){
			close(fd_orig);
			unlink(orig);
		}
		if (fd_dest>=0)
			close(fd_dest);
	}
	return ok;
}