/*
 * options.c -- options functions.
 *
 * Copyright (c) 2001-2006, NLnet Labs. All rights reserved.
 *
 * See LICENSE for the license.
 *
 */
#include <config.h>
#include <string.h>
#include <errno.h>
#include "options.h"
#include "query.h"
#include "tsig.h"

#include "configyyrename.h"
#include "configparser.h"
nsd_options_t* nsd_options = 0;
config_parser_state_t* cfg_parser = 0;
extern FILE* c_in, *c_out;
int c_parse(void);
int c_lex(void);
int c_wrap(void);
void c_error(const char *message);

nsd_options_t* nsd_options_create(region_type* region)
{
	nsd_options_t* opt;
	opt = (nsd_options_t*)region_alloc(region, sizeof(nsd_options_t));
	opt->region = region;
	opt->zone_options = rbtree_create(region, 
		(int (*)(const void *, const void *)) dname_compare);
	opt->keys = NULL;
	opt->numkeys = 0;
	opt->ip_addresses = NULL;
	opt->debug_mode = 0;
	opt->ip4_only = 0;
	opt->ip6_only = 0;
	opt->database = DBFILE;
	opt->identity = IDENTITY;
	opt->logfile = 0;
	opt->server_count = 1;
	opt->tcp_count = 10;
	opt->pidfile = PIDFILE;
	opt->port = UDP_PORT;
/* deprecated?	opt->port = TCP_PORT; */
	opt->statistics = 0;
	opt->chroot = 0;
	opt->username = USER;
	opt->zonesdir = 0;
	opt->difffile = 0;
	opt->xfrdfile = 0;
	opt->xfrd_reload_timeout = 10;
	nsd_options = opt;
	return opt;
}

int nsd_options_insert_zone(nsd_options_t* opt, zone_options_t* zone)
{
	/* create dname for lookup */
	const dname_type* dname = dname_parse(opt->region, zone->name);
	if(!dname) 
		return 0;
	zone->node.key = dname;
	if(!rbtree_insert(opt->zone_options, (rbnode_t*)zone))
		return 0;
	return 1;
}

int parse_options_file(nsd_options_t* opt, const char* file)
{
	FILE *in = 0;
	zone_options_t* zone;
	acl_options_t* acl;

	if(!cfg_parser) 
		cfg_parser = (config_parser_state_t*)region_alloc(
			opt->region, sizeof(config_parser_state_t));
	cfg_parser->filename = file;
	cfg_parser->line = 1;
	cfg_parser->errors = 0;
	cfg_parser->opt = opt;
	cfg_parser->current_zone = 0;
	cfg_parser->current_key = opt->keys;
	while(cfg_parser->current_key && cfg_parser->current_key->next)
		cfg_parser->current_key = cfg_parser->current_key->next;
	cfg_parser->current_ip_address_option = opt->ip_addresses;
	while(cfg_parser->current_ip_address_option && cfg_parser->current_ip_address_option->next)
		cfg_parser->current_ip_address_option = cfg_parser->current_ip_address_option->next;
	cfg_parser->current_allow_notify = 0;
	cfg_parser->current_request_xfr = 0;
	cfg_parser->current_notify = 0;
	cfg_parser->current_provide_xfr = 0;

	in = fopen(cfg_parser->filename, "r");
	if(!in) {
		fprintf(stderr, "Could not open %s: %s\n", file, strerror(errno));
		return 0;
	}
	c_in = in;
        c_parse();
	fclose(in);

	if(cfg_parser->current_zone) {
		if(!cfg_parser->current_zone->name) 
			c_error("last zone has no name");
		else {
			if(!nsd_options_insert_zone(opt, 
				cfg_parser->current_zone))
				c_error("duplicate zone");
		}
		if(!cfg_parser->current_zone->zonefile) 
			c_error("last zone has no zonefile");
	}
	if(opt->keys)
	{
		if(!opt->keys->name) 
			c_error("last key has no name");
		if(!opt->keys->algorithm) 
			c_error("last key has no algorithm");
		if(!opt->keys->secret) 
			c_error("last key has no secret blob");
	}
	RBTREE_FOR(zone, zone_options_t*, opt->zone_options)
	{
		if(!zone->name) 
			continue;
		if(!zone->zonefile) 
			continue;
		/* lookup keys for acls */
		for(acl=zone->allow_notify; acl; acl=acl->next)
		{
			if(acl->nokey || acl->blocked) 
				continue;
			acl->key_options = key_options_find(opt, acl->key_name);
			if(!acl->key_options) 
				c_error_msg("key %s in zone %s could not be found",
					acl->key_name, zone->name);
		}
		for(acl=zone->notify; acl; acl=acl->next)
		{
			if(acl->nokey || acl->blocked) 
				continue;
			acl->key_options = key_options_find(opt, acl->key_name);
			if(!acl->key_options) 
				c_error_msg("key %s in zone %s could not be found",
					acl->key_name, zone->name);
		}
		for(acl=zone->request_xfr; acl; acl=acl->next)
		{
			if(acl->nokey || acl->blocked) 
				continue;
			acl->key_options = key_options_find(opt, acl->key_name);
			if(!acl->key_options) 
				c_error_msg("key %s in zone %s could not be found",
					acl->key_name, zone->name);
		}
		for(acl=zone->provide_xfr; acl; acl=acl->next)
		{
			if(acl->nokey || acl->blocked) 
				continue;
			acl->key_options = key_options_find(opt, acl->key_name);
			if(!acl->key_options) 
				c_error_msg("key %s in zone %s could not be found",
					acl->key_name, zone->name);
		}
	}
	
	if(cfg_parser->errors > 0)
	{
        	fprintf(stderr, "read %s failed: %d errors in configuration file\n", 
			cfg_parser->filename,
			cfg_parser->errors);
		return 0;
	}
	return 1;
}

void c_error_va_list(const char *fmt, va_list args)
{
	cfg_parser->errors++;
        fprintf(stderr, "%s:%d: error: ", cfg_parser->filename,
		cfg_parser->line);
	vfprintf(stderr, fmt, args);
	fprintf(stderr, "\n");
}

void c_error_msg(const char* fmt, ...)
{
        va_list args;
        va_start(args, fmt);
        c_error_va_list(fmt, args);
        va_end(args);
}

void c_error(const char *str)
{
	cfg_parser->errors++;
        fprintf(stderr, "%s:%d: error: %s\n", cfg_parser->filename,
		cfg_parser->line, str);
}

int c_wrap()
{
        return 1;
}

zone_options_t* zone_options_create(region_type* region)
{
	zone_options_t* zone;
	zone = (zone_options_t*)region_alloc(region, sizeof(zone_options_t));
	zone->node = *RBTREE_NULL;
	zone->name = 0;
	zone->zonefile = 0;
	zone->allow_notify = 0;
	zone->request_xfr = 0;
	zone->notify = 0;
	zone->provide_xfr = 0;
	zone->zone_is_ok = 0;
	return zone;
}

key_options_t* key_options_create(region_type* region)
{
	key_options_t* key;
	key = (key_options_t*)region_alloc(region, sizeof(key_options_t));
	key->name = 0;
	key->next = 0;
	key->algorithm = 0;
	key->secret = 0;
#ifdef TSIG
	key->tsig_key = 0;
#endif
	return key;
}

key_options_t* key_options_find(nsd_options_t* opt, const char* name)
{
	key_options_t* key = opt->keys;
	while(key) {
		if(strcmp(key->name, name)==0) 
			return key;
		key = key->next;
	}
	return 0;
}

int acl_check_incoming(acl_options_t* acl, struct query* q, 
	acl_options_t** reason)
{
	/* check each acl element.
	   if 1 blocked element matches - return -1.
	   if any element matches - return number.
	   else return -1. */
	int found_match = -1;
	int number = 0;
	acl_options_t* match = 0;

	if(reason)
		*reason = NULL;

#ifdef TSIG
	tsig_init_record(&q->tsig, q->region, NULL, NULL);
	if(!tsig_find_rr(&q->tsig, q->packet)) {
		log_msg(LOG_ERR, "bad tsig RR format");
		return -1;
	}
#endif /* TSIG */

	while(acl)
	{
		log_msg(LOG_INFO, "testing acl %s %s",
			acl->ip_address_spec, acl->nokey?"NOKEY":
			(acl->blocked?"BLOCKED":acl->key_name));
		if(acl_addr_matches(acl, q) && acl_key_matches(acl, q)) {
			if(!match)
				match = acl; /* remember first match */
			if(acl->blocked) {
				if(reason)
					*reason = acl;
				return -1;
			}
			found_match=number;
		}
		number++;
		acl = acl->next;
	}

#ifdef TSIG
	if(match && !match->nokey && !match->blocked) {
		/* check TSIG */
		log_msg(LOG_INFO, "TSIG check for match %s %s",
			match->ip_address_spec, match->key_name);
		if(q->tsig.status != TSIG_OK) {
			log_msg(LOG_ERR, "query has no tsig");
			return -1;
		}
		if(!tsig_from_query(&q->tsig)) {
			log_msg(LOG_ERR, "query tsig unknown key/algorithm");
			return -1;
		}
		if(q->tsig.key != match->key_options->tsig_key) {
			log_msg(LOG_ERR, "query tsig wrong key");
			return -1;
		}
		if(strcmp(q->tsig.algorithm->short_name,
			match->key_options->algorithm) != 0) {
			return -1;
		}
		buffer_set_limit(q->packet, q->tsig.position);
		ARCOUNT_SET(q->packet, ARCOUNT(q->packet) - 1);
		tsig_prepare(&q->tsig);
		tsig_update(&q->tsig, q->packet, buffer_limit(q->packet));
		if(!tsig_verify(&q->tsig)) {
			log_msg(LOG_ERR, "query bad tsig signature");
			return -1;
		}
		log_msg(LOG_INFO, "query good tsig signature");
	}
#endif /* TSIG */

	if(reason)
		*reason = match;
	return found_match;
}

int acl_addr_matches(acl_options_t* acl, struct query* q)
{
	if(acl->is_ipv6)
	{
#ifdef INET6
		struct sockaddr_storage* addr_storage = (struct sockaddr_storage*)&q->addr;
		struct sockaddr_in6* addr = (struct sockaddr_in6*)&q->addr;
		if(addr_storage->ss_family != AF_INET6) 
			return 0;
		if(acl->port != 0 && acl->port != addr->sin6_port) 
			return 0;
		switch(acl->rangetype) {
		case acl_range_mask:
		case acl_range_subnet:
			if(!acl_addr_match_mask((uint32_t*)&acl->addr.addr6, (uint32_t*)&addr->sin6_addr,
				(uint32_t*)&acl->range_mask.addr6, sizeof(struct in6_addr)))
				return 0;
			break;
		case acl_range_minmax:
			if(!acl_addr_match_range((uint32_t*)&acl->addr.addr6, (uint32_t*)&addr->sin6_addr,
				(uint32_t*)&acl->range_mask.addr6, sizeof(struct in6_addr)))
				return 0;
			break;
		case acl_range_single:
		default:
			if(memcmp(&addr->sin6_addr, &acl->addr.addr6, 
				sizeof(struct in6_addr)) != 0)
				return 0;
			break;
		}
		return 1;
#else
		return 0; /* no inet6, no match */
#endif
	}
	else
	{
		struct sockaddr_in* addr = (struct sockaddr_in*)&q->addr;
		if(addr->sin_family != AF_INET) 
			return 0;
		if(acl->port != 0 && acl->port != addr->sin_port) 
			return 0;
		switch(acl->rangetype) {
		case acl_range_mask:
		case acl_range_subnet:
			if(!acl_addr_match_mask((uint32_t*)&acl->addr.addr, (uint32_t*)&addr->sin_addr,
				(uint32_t*)&acl->range_mask.addr, sizeof(struct in_addr)))
				return 0;
			break;
		case acl_range_minmax:
			if(!acl_addr_match_range((uint32_t*)&acl->addr.addr, (uint32_t*)&addr->sin_addr,
				(uint32_t*)&acl->range_mask.addr, sizeof(struct in_addr)))
				return 0;
			break;
		case acl_range_single:
		default:
			if(memcmp(&addr->sin_addr, &acl->addr.addr, 
				sizeof(struct in_addr)) != 0)
				return 0;
			break;
		}
		return 1;
	}
	/* ENOTREACH */
	return 0;
}

int acl_addr_match_mask(uint32_t* a, uint32_t* b, uint32_t* mask, size_t sz)
{
	size_t i;
#ifndef NDEBUG
	assert(sz % 4 == 0);
#endif
	sz /= 4;
	for(i=0; i<sz; ++i)
	{
		if(((*a++)&*mask) != ((*b++)&*mask))
			return 0;
		++mask;
	}
	return 1;
}

int acl_addr_match_range(uint32_t* minval, uint32_t* x, uint32_t* maxval, size_t sz)
{
	size_t i;
	uint8_t checkmin = 1, checkmax = 1;
#ifndef NDEBUG
	assert(sz % 4 == 0);
#endif
	/* check treats x as one huge number */
	sz /= 4;
	for(i=0; i<sz; ++i)
	{
		/* if outside bounds, we are done */
		if(checkmin)
			if(minval[i] > x[i]) 
				return 0;
		if(checkmax)
			if(maxval[i] < x[i]) 
				return 0;
		/* if x is equal to a bound, that bound needs further checks */
		if(checkmin && minval[i]!=x[i])
			checkmin = 0;
		if(checkmax && maxval[i]!=x[i])
			checkmax = 0;
		if(!checkmin && !checkmax) 
			return 1; /* will always match */
	}
	return 1;
}

int acl_key_matches(acl_options_t* acl, struct query* q)
{
	if(acl->nokey) 
		return 1;
	if(acl->blocked) 
		return 1;
	/* check name of tsig key */
#ifdef TSIG
	if(q->tsig.status != TSIG_OK) {
		log_msg(LOG_INFO, "keymatch fail query has no TSIG");
		return 0; /* query has no TSIG */
	}
	if(!acl->key_options->tsig_key) {
		log_msg(LOG_INFO, "keymatch fail no config");
		return 0; /* key not properly configged */
	}
	if(dname_compare(q->tsig.key_name, 
		acl->key_options->tsig_key->name) != 0) {
		log_msg(LOG_INFO, "keymatch fail wrong key name");
		return 0; /* wrong key name */
	}
	/* algorithm is checked in tsig_from_query() call later */
	return 1;
#else
	return 0;
#endif
}

int 
acl_same_host(acl_options_t* a, acl_options_t* b)
{
	if(a->is_ipv6 && !b->is_ipv6)
		return 0;
	if(!a->is_ipv6 && b->is_ipv6)
		return 0;
	if(a->port != b->port)
		return 0;
	if(a->rangetype != b->rangetype)
		return 0;
	if(!a->is_ipv6) {
		if(memcmp(&a->addr.addr, &b->addr.addr, 
		   sizeof(struct in_addr)) != 0)
			return 0;
		if(a->rangetype != acl_range_single &&
		   memcmp(&a->range_mask.addr, &b->range_mask.addr, 
		   sizeof(struct in_addr)) != 0)
			return 0;
	} else {
#ifdef INET6
		if(memcmp(&a->addr.addr6, &b->addr.addr6, 
		   sizeof(struct in6_addr)) != 0)
			return 0;
		if(a->rangetype != acl_range_single &&
		   memcmp(&a->range_mask.addr6, &b->range_mask.addr6, 
		   sizeof(struct in6_addr)) != 0)
			return 0;
#else
		return 0;
#endif
	}
	return 1;
}

void key_options_tsig_add(nsd_options_t* opt)
{
#if defined(TSIG) && defined(HAVE_SSL)
	key_options_t* optkey;
	uint8_t data[4000];
	tsig_key_type* tsigkey;
	const dname_type* dname;
	int size;

	for(optkey = opt->keys; optkey; optkey = optkey->next)
	{
		dname = dname_parse(opt->region, optkey->name);
		if(!dname) {
			log_msg(LOG_ERR, "Failed to parse tsig key name %s", optkey->name);
			continue;
		}
		size = b64_pton(optkey->secret, data, sizeof(data));
		if(size == -1) {
			log_msg(LOG_ERR, "Failed to parse tsig key data %s", optkey->name);
			continue;
		}
		tsigkey = (tsig_key_type *) region_alloc(opt->region, sizeof(tsig_key_type));
		tsigkey->name = dname;
		tsigkey->size = size;
		tsigkey->data = (uint8_t *) region_alloc_init(opt->region, data, tsigkey->size);
		tsig_add_key(tsigkey);
		optkey->tsig_key = tsigkey;
		log_msg(LOG_INFO, "tsig key '%s' added.", 
			dname_to_string(tsigkey->name, NULL));
	}
#endif
}

int zone_is_slave(zone_options_t* opt)
{
	return opt->request_xfr != 0;
}

zone_options_t* zone_options_find(nsd_options_t* opt, const struct dname* apex)
{
	return (zone_options_t*) rbtree_search(opt->zone_options, apex);
}