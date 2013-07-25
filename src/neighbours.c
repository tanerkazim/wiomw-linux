#include <config.h>
#include "neighbours.h"
#include "print_error.h"
#include "sockaddr_helpers.h"
#include "configuration.h"
#include "mac_ntop.h"
#include <stdio.h>
#include <libmnl/libmnl.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/if.h>
#include <asm/types.h>
#include <sys/socket.h>
#include <sysexits.h>
#include <regex.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <inttypes.h>

#define UNIVERSAL_IFACE_BLACKLIST_REGEX "^lo$"
#define UNIVERSAL_IFACE_BLACKLIST_REGEX_LENGTH 4


/* to go in config */
#define SHOW_BLACKLISTED 0
#define SHOW_NOT_RUNNING 0
#define SHOW_SECONDARY 0
#define SHOW_BLACKLISTED_INTERFACE_NEIGHBOURS 0



typedef struct {
	unsigned char family;
	unsigned char mask;
	unsigned char flags;
	unsigned char scope;
	struct sockaddr* addr;
	struct sockaddr* local;
	char* label;
	struct sockaddr* bcast;
	struct sockaddr* acast;
} if_addr_t;

typedef struct if_addr_list_struct {
	if_addr_t* data;
	struct if_addr_list_struct* next;
} * if_addr_list_t;

typedef struct {
	unsigned char family;
	unsigned short type;
	int index;
	unsigned int flags;
	char* name;
	unsigned char mac[6];
	unsigned char bmac[6];
	uint32_t mtu;
	int link;
	char* qdsp;
	bool blacklisted;
	if_addr_list_t addr_list;
} if_item_t;

typedef struct {
	regex_t* compiled_regex;
	if_item_t* if_item;
} get_if_item_cb_data_t;

typedef struct if_list_struct {
	if_item_t* data;
	struct if_list_struct* next;
} * if_list_t;

typedef struct {
	regex_t* compiled_regex;
	if_list_t* if_list;
} get_if_list_cb_data_t;

typedef struct {
	FILE* fd;
	if_list_t* if_list;
} print_neigh_list_cb_data_t;

typedef struct {
	FILE* fd;
	unsigned char family;
} print_neigh_cb_data_t;






static if_addr_t* new_if_addr()
{
	if_addr_t* if_addr = (if_addr_t*)malloc(sizeof(if_addr_t));
	if (if_addr == NULL) {
		print_syserror("Unable to allocate memory for an interface address object");
		exit(EX_OSERR);
	} else {
		if_addr->label = NULL;
		if_addr->addr = NULL;
		if_addr->local = NULL;
		if_addr->bcast = NULL;
		return if_addr;
	}
}

static void destroy_if_addr(if_addr_t** if_addr)
{
	if (if_addr != NULL && *if_addr != NULL) {
		free((*if_addr)->label);
		free((*if_addr)->addr);
		free((*if_addr)->local);
		free((*if_addr)->bcast);
		free(*if_addr);
		*if_addr = NULL;
	}
}


static if_addr_list_t new_if_addr_list()
{
	return (if_addr_list_t)NULL;
}

static void destroy_if_addr_list(if_addr_list_t* if_addr_list)
{
	while (if_addr_list != NULL && *if_addr_list != NULL) {
		if_addr_list_t temp = *if_addr_list;
		destroy_if_addr(&(temp->data));
		*if_addr_list = temp->next;
		free(temp);
	}
}


static if_item_t* new_if_item()
{
	if_item_t* if_item = (if_item_t*)malloc(sizeof(if_item_t));
	if (if_item == NULL) {
		print_syserror("Unable to allocate memory for an interface object");
		exit(EX_OSERR);
	}
	if_item->name = NULL;
	if_item->qdsp = NULL;
	if_item->addr_list = new_if_addr_list();
	return if_item;
}

static void destroy_if_item(if_item_t** if_item)
{
	if (if_item == NULL || *if_item == NULL) {
		print_error("Unable to destroy an empty interface object");
	} else {
		free((*if_item)->name);
		free((*if_item)->qdsp);
		destroy_if_addr_list(&((*if_item)->addr_list));
		free(*if_item);
		*if_item = NULL;
	}
}


static get_if_item_cb_data_t new_get_if_item_cb_data()
{
	get_if_item_cb_data_t result;
	return result;
}

static void destroy_get_if_item_cb_data(get_if_item_cb_data_t* get_if_item_cb_data)
{
	if (get_if_item_cb_data == NULL) {
		print_error("Unable to destroy an empty interface item callback argument");
	}
}


static if_list_t new_if_list()
{
	return (if_list_t)NULL;
}

static void destroy_if_list(if_list_t* if_list)
{
	while (if_list != NULL && *if_list != NULL) {
		if_list_t temp = *if_list;
		destroy_if_item(&(temp->data));
		*if_list = temp->next;
		free(temp);
	}
}



static get_if_list_cb_data_t new_get_if_list_cb_data(regex_t* compiled_regex, if_list_t* if_list)
{
	get_if_list_cb_data_t result;
	result.compiled_regex = compiled_regex;
	result.if_list = if_list;
	return result;
}

static void destroy_get_if_list_cb_data(get_if_list_cb_data_t* get_if_list_cb_data)
{
	if (get_if_list_cb_data == NULL) {
		print_error("Unable to destroy an empty interface list callback argument");
	} else {
		get_if_list_cb_data->compiled_regex = NULL;
		get_if_list_cb_data->if_list = NULL;
	}
}


static print_neigh_list_cb_data_t new_print_neigh_list_cb_data(FILE* fd, if_list_t* if_list)
{
	print_neigh_list_cb_data_t result;
	result.fd = fd;
	result.if_list = if_list;
	return result;
}

static void destroy_print_neigh_list_cb_data(print_neigh_list_cb_data_t* print_neigh_list_cb_data)
{
	if (print_neigh_list_cb_data == NULL) {
		print_error("Unable to destroy an empty print neighbour list callback argument");
	}
}


static print_neigh_cb_data_t new_print_neigh_cb_data(FILE* fd, unsigned char family)
{
	print_neigh_cb_data_t result;
	result.fd = fd;
	result.family = family;
	return result;
}

static void destroy_print_neigh_cb_data(print_neigh_cb_data_t* print_neigh_cb_data)
{
	if (print_neigh_cb_data == NULL) {
		print_error("Unable to destroy an empty print neighbour callback argument");
	}
}







static if_addr_list_t push_if_addr(if_addr_list_t* if_addr_list, if_addr_t* if_addr)
{
	if_addr_list_t temp = (if_addr_list_t)malloc(sizeof(struct if_addr_list_struct));
	if (temp == NULL) {
		print_syserror("Unable to allocate interface address list entry");
		exit(EX_OSERR);
	}

	temp->data = if_addr;
	temp->next = *if_addr_list;
	*if_addr_list = temp;
	return *if_addr_list;
}

/*
static if_addr_t* pop_if_addr(if_addr_list_t* if_addr_list)
{
	if_addr_list_t temp = *if_addr_list;
	if_addr_t* if_addr = temp->data;
	*if_addr_list = temp->next;
	free(temp);
	return if_addr;
}
*/

static if_list_t push_if_item(if_list_t* if_list, if_item_t* if_item)
{
	if_list_t temp = (if_list_t)malloc(sizeof(struct if_list_struct));
	if (temp == NULL) {
		print_syserror("Unable to allocate interface list entry");
		exit(EX_OSERR);
	}
	temp->data = if_item;
	temp->next = *if_list;
	*if_list = temp;
	return *if_list;
}

/*
static if_item_t* pop_if_item(if_list_t* if_list)
{
	if_list_t temp = *if_list;
	if_item_t* if_item = temp->data;
	*if_list = temp->next;
	free(temp);
	return if_item;
}
*/


static if_item_t* get_if_item_by_index(const if_list_t if_list, const int if_index)
{
	if_list_t temp = if_list;
	while (temp != NULL) {
		if (temp->data->index == if_index) {
			return temp->data;
		} else {
			temp = temp->next;
		}
	}
	return (if_item_t*)NULL;
}








static int get_if_item_cb(const struct nlattr* nl_attr, void* cb_data)
{
	get_if_item_cb_data_t* item_cb_data = (get_if_item_cb_data_t*)cb_data;
	if (item_cb_data == NULL || item_cb_data->compiled_regex == NULL || item_cb_data->if_item == NULL) {
		print_error("Inavlid argument received in interface item callback");
		return MNL_CB_ERROR;
	} else if (mnl_attr_type_valid(nl_attr, IFLA_MAX) < 0) {
		print_syserror("Received invalid netlink attribute type");
		return MNL_CB_ERROR;
	} else if (!SHOW_BLACKLISTED && item_cb_data->if_item->blacklisted) {
		return MNL_CB_OK;
	} else {
		switch (mnl_attr_get_type(nl_attr)) {
		case IFLA_IFNAME:
			if (mnl_attr_validate(nl_attr, MNL_TYPE_NUL_STRING) < 0) {
				print_syserror("Received invalid interface name from netlink");
				return MNL_CB_ERROR;
			} else {
				int errcode = 0;
				const char* if_name = mnl_attr_get_str(nl_attr);
				item_cb_data->if_item->name = (char*)malloc(strlen(if_name) + 1);
				if (item_cb_data->if_item->name == NULL) {
					print_syserror("Unable to allocate interface name");
					exit(EX_OSERR);
				}
				strcpy(item_cb_data->if_item->name, if_name);
				errcode = regexec(item_cb_data->compiled_regex, item_cb_data->if_item->name, 0, NULL, REG_EXTENDED);
				if (errcode == 0) {
					item_cb_data->if_item->blacklisted = true;
				} else if (errcode == REG_NOMATCH) {
					item_cb_data->if_item->blacklisted = false;
				} else {
					print_syserror("Unable to evaluate the interface blacklist regex");
					return MNL_CB_ERROR;
				}
			}
			break;
		case IFLA_ADDRESS:
			if (mnl_attr_validate2(nl_attr, MNL_TYPE_BINARY, 6) < 0) {
				print_syserror("Received invalid MAC address from netlink");
				return MNL_CB_ERROR;
			} else {
				const unsigned char* mac = mnl_attr_get_payload(nl_attr);
				memcpy(item_cb_data->if_item->mac, mac, 6);
			}
			break;
		case IFLA_BROADCAST:
			if (mnl_attr_validate2(nl_attr, MNL_TYPE_BINARY, 6) < 0) {
				print_syserror("Received invalid broadcast MAC address from netlink");
				return MNL_CB_ERROR;
			} else {
				const unsigned char* bmac = mnl_attr_get_payload(nl_attr);
				memcpy(item_cb_data->if_item->bmac, bmac, 6);
			}
			break;
		case IFLA_MTU:
			if (mnl_attr_validate(nl_attr, MNL_TYPE_U32) < 0) {
				print_syserror("Received invalid MTU from netlink");
				return MNL_CB_ERROR;
			} else {
				item_cb_data->if_item->mtu = mnl_attr_get_u32(nl_attr);
			}
			break;
		case IFLA_LINK:
			if (mnl_attr_validate2(nl_attr, MNL_TYPE_BINARY, sizeof(int)) < 0) {
				print_syserror("Received invalid link type from netlink");
				return MNL_CB_ERROR;
			} else {
				int* link_ptr = mnl_attr_get_payload(nl_attr);
				item_cb_data->if_item->link = *link_ptr;
			}
			break;
		case IFLA_QDISC:
			if (mnl_attr_validate(nl_attr, MNL_TYPE_NUL_STRING) < 0) {
				print_syserror("Received invalid queue discipline from netlink");
				return MNL_CB_ERROR;
			} else {
				const char* qdsp = mnl_attr_get_str(nl_attr);
				item_cb_data->if_item->qdsp = (char*)malloc(strlen(qdsp) + 1);
				if (item_cb_data->if_item->qdsp == NULL) {
					print_syserror("Unable to allocate interface queue discipline name");
					exit(EX_OSERR);
				}
				strcpy(item_cb_data->if_item->qdsp, qdsp);
			}
			break;
		}
		return MNL_CB_OK;
	}
}

static int get_if_addr_cb(const struct nlattr* nl_attr, void* cb_data)
{
	if_addr_t* if_addr = (if_addr_t*)cb_data;
	if (if_addr == NULL) {
		print_error("Invalid argument received in interface address callback ");
		return MNL_CB_ERROR;
	} else if (mnl_attr_type_valid(nl_attr, IFA_MAX) < 0) {
		print_syserror("Received invalid netlink attribute type");
		return MNL_CB_ERROR;
	} else {
		switch(mnl_attr_get_type(nl_attr)) {
		case IFA_ADDRESS:
			if (if_addr->family == AF_INET && mnl_attr_validate2(nl_attr, MNL_TYPE_BINARY, sizeof(struct in_addr)) >= 0) {
				struct in_addr* bin_addr = mnl_attr_get_payload(nl_attr);
				struct sockaddr_in* addr = (struct sockaddr_in*)malloc(sizeof(struct sockaddr_in));
				if (addr == NULL) {
					print_syserror("Unable to allocate memory for a socket address");
					exit(EX_OSERR);
				}
				memset(addr, 0, sizeof(struct sockaddr_in));
				addr->sin_family = AF_INET;
				memcpy(&(addr->sin_addr.s_addr), bin_addr, sizeof(struct in_addr));
				if_addr->addr = (struct sockaddr*)addr;
			} else if (if_addr->family == AF_INET6 && mnl_attr_validate2(nl_attr, MNL_TYPE_BINARY, sizeof(struct in6_addr)) >= 0) {
				struct in6_addr* bin6_addr = mnl_attr_get_payload(nl_attr);
				struct sockaddr_in6* addr = (struct sockaddr_in6*)malloc(sizeof(struct sockaddr_in6));
				if (addr == NULL) {
					print_syserror("Unable to allocate memory for a socket address");
					exit(EX_OSERR);
				}
				memset(addr, 0, sizeof(struct sockaddr_in6));
				addr->sin6_family = AF_INET6;
				memcpy(&(addr->sin6_addr.s6_addr), bin6_addr, sizeof(struct in6_addr));
				if_addr->addr = (struct sockaddr*)addr;
			} else {
				print_syserror("Received invalid address from netlink");
				return MNL_CB_ERROR;
			}
			break;
		case IFA_LOCAL:
			if (if_addr->family == AF_INET && mnl_attr_validate2(nl_attr, MNL_TYPE_BINARY, sizeof(struct in_addr)) >= 0) {
				struct in_addr* bin_addr = mnl_attr_get_payload(nl_attr);
				struct sockaddr_in* addr = (struct sockaddr_in*)malloc(sizeof(struct sockaddr_in));
				if (addr == NULL) {
					print_syserror("Unable to allocate memory for a socket address");
					exit(EX_OSERR);
				}
				memset(addr, 0, sizeof(struct sockaddr_in));
				addr->sin_family = AF_INET;
				memcpy(&(addr->sin_addr.s_addr), bin_addr, sizeof(struct in_addr));
				if_addr->local = (struct sockaddr*)addr;
			} else if (if_addr->family == AF_INET6 && mnl_attr_validate2(nl_attr, MNL_TYPE_BINARY, sizeof(struct in6_addr)) >= 0) {
				struct in6_addr* bin6_addr = mnl_attr_get_payload(nl_attr);
				struct sockaddr_in6* addr = (struct sockaddr_in6*)malloc(sizeof(struct sockaddr_in6));
				if (addr == NULL) {
					print_syserror("Unable to allocate memory for a socket address");
					exit(EX_OSERR);
				}
				memset(addr, 0, sizeof(struct sockaddr_in6));
				addr->sin6_family = AF_INET6;
				memcpy(&(addr->sin6_addr.s6_addr), bin6_addr, sizeof(struct in6_addr));
				if_addr->local = (struct sockaddr*)addr;
			} else {
				print_syserror("Received invalid local address from netlink");
				return MNL_CB_ERROR;
			}
			break;
		case IFA_LABEL:
			if (mnl_attr_validate(nl_attr, MNL_TYPE_NUL_STRING) < 0) {
				print_syserror("Received invalid address label from netlink");
				return MNL_CB_ERROR;
			} else {
				const char* label = mnl_attr_get_payload(nl_attr);
				if_addr->label = (char*)malloc(strlen(label) + 1);
				if (if_addr->label == NULL) {
					print_syserror("Unable to allocate memory for an address label");
					exit(EX_OSERR);
				}
				strcpy(if_addr->label, label);
			}
			break;
		case IFA_BROADCAST:
			if (if_addr->family == AF_INET && mnl_attr_validate2(nl_attr, MNL_TYPE_BINARY, sizeof(struct in_addr)) >= 0) {
				struct in_addr* bin_addr = mnl_attr_get_payload(nl_attr);
				struct sockaddr_in* addr = (struct sockaddr_in*)malloc(sizeof(struct sockaddr_in));
				if (addr == NULL) {
					print_syserror("Unable to allocate memory for a socket address");
					exit(EX_OSERR);
				}
				memset(addr, 0, sizeof(struct sockaddr_in));
				addr->sin_family = AF_INET;
				memcpy(&(addr->sin_addr.s_addr), bin_addr, sizeof(struct in_addr));
				if_addr->local = (struct sockaddr*)addr;
			} else if (if_addr->family == AF_INET6 && mnl_attr_validate2(nl_attr, MNL_TYPE_BINARY, sizeof(struct in6_addr)) >= 0) {
				struct in6_addr* bin6_addr = mnl_attr_get_payload(nl_attr);
				struct sockaddr_in6* addr = (struct sockaddr_in6*)malloc(sizeof(struct sockaddr_in6));
				if (addr == NULL) {
					print_syserror("Unable to allocate memory for a socket address");
					exit(EX_OSERR);
				}
				memset(addr, 0, sizeof(struct sockaddr_in6));
				addr->sin6_family = AF_INET6;
				memcpy(&(addr->sin6_addr.s6_addr), bin6_addr, sizeof(struct in6_addr));
				if_addr->local = (struct sockaddr*)addr;
			} else {
				print_syserror("Received invalid broadcast address from netlink");
				return MNL_CB_ERROR;
			}
			break;
		case IFA_ANYCAST:
			if (if_addr->family == AF_INET && mnl_attr_validate2(nl_attr, MNL_TYPE_BINARY, sizeof(struct in_addr)) >= 0) {
				struct in_addr* bin_addr = mnl_attr_get_payload(nl_attr);
				struct sockaddr_in* addr = (struct sockaddr_in*)malloc(sizeof(struct sockaddr_in));
				if (addr == NULL) {
					print_syserror("Unable to allocate memory for a socket address");
					exit(EX_OSERR);
				}
				memset(addr, 0, sizeof(struct sockaddr_in));
				addr->sin_family = AF_INET;
				memcpy(&(addr->sin_addr.s_addr), bin_addr, sizeof(struct in_addr));
				if_addr->local = (struct sockaddr*)addr;
			} else if (if_addr->family == AF_INET6 && mnl_attr_validate2(nl_attr, MNL_TYPE_BINARY, sizeof(struct in6_addr)) >= 0) {
				struct in6_addr* bin6_addr = mnl_attr_get_payload(nl_attr);
				struct sockaddr_in6* addr = (struct sockaddr_in6*)malloc(sizeof(struct sockaddr_in6));
				if (addr == NULL) {
					print_syserror("Unable to allocate memory for a socket address");
					exit(EX_OSERR);
				}
				memset(addr, 0, sizeof(struct sockaddr_in6));
				addr->sin6_family = AF_INET6;
				memcpy(&(addr->sin6_addr.s6_addr), bin6_addr, sizeof(struct in6_addr));
				if_addr->local = (struct sockaddr*)addr;
			} else {
				print_syserror("Received invalid anycast address from netlink");
				return MNL_CB_ERROR;
			}
			break;
		}
		return MNL_CB_OK;
	}
}

static int print_neigh_cb(const struct nlattr* nl_attr, void* cb_data)
{
	print_neigh_cb_data_t* neigh_cb_data = (print_neigh_cb_data_t*)cb_data;
	if (neigh_cb_data == NULL || neigh_cb_data->fd == NULL) {
		print_error("Invalid argument received in print neighbour callback");
		return MNL_CB_ERROR;
	} else if (mnl_attr_type_valid(nl_attr, NDA_MAX) < 0) {
		print_syserror("Received invalid netlink attribute type");
		return MNL_CB_ERROR;
	} else {
		switch(mnl_attr_get_type(nl_attr)) {
		case NDA_DST:
			if (neigh_cb_data->family == AF_INET && mnl_attr_validate2(nl_attr, MNL_TYPE_BINARY, sizeof(struct in_addr)) >= 0) {
				struct in_addr* bin_addr = mnl_attr_get_payload(nl_attr);
				char str_temp[BUFSIZ];
				if (NULL == inet_ntop(AF_INET, bin_addr, str_temp, sizeof(struct in_addr))) {
					print_syserror("Unable to print destination address");
					return MNL_CB_ERROR;
				}
				fprintf(neigh_cb_data->fd, "\"ipaddress\":\"%s\",", str_temp);
			} else if (neigh_cb_data->family == AF_INET6 && mnl_attr_validate2(nl_attr, MNL_TYPE_BINARY, sizeof(struct in6_addr)) >= 0) {
				struct in6_addr* bin6_addr = mnl_attr_get_payload(nl_attr);
				char str_temp[BUFSIZ];
				if (NULL == inet_ntop(AF_INET6, bin6_addr, str_temp, sizeof(struct in6_addr))) {
					print_syserror("Unable to print destination address");
					return MNL_CB_ERROR;
				}
				fprintf(neigh_cb_data->fd, "\"ip6\":\"%s\",", str_temp);
			} else {
				print_syserror("Received invalid destination address from netlink");
				return MNL_CB_ERROR;
			}
			break;
		case NDA_LLADDR:
			if (mnl_attr_validate2(nl_attr, MNL_TYPE_BINARY, 6) < 0) {
				print_syserror("Received invalid MAC address from netlink");
				return MNL_CB_ERROR;
			} else {
				const unsigned char* mac = mnl_attr_get_payload(nl_attr);
				char str_temp[BUFSIZ];
				if (NULL == mac_ntop(mac, str_temp, 6)) {
					print_error("Unable to print MAC address");
					return MNL_CB_ERROR;
				}
				fprintf(neigh_cb_data->fd, "\"macaddress\":\"%s\",", str_temp);
			}
			break;
		}
		return MNL_CB_OK;
	}
}

static int get_if_list_cb(const struct nlmsghdr* nl_head, void* cb_data)
{
	get_if_list_cb_data_t* list_cb_data = (get_if_list_cb_data_t*)cb_data;
	if (list_cb_data == NULL || list_cb_data->compiled_regex == NULL) {
		print_error("Invalid argument received in interface list callback");
		return MNL_CB_ERROR;
	} else {
		struct ifinfomsg* if_msg = mnl_nlmsg_get_payload(nl_head);
	
		if (!(if_msg->ifi_flags & IFF_LOOPBACK) && (SHOW_NOT_RUNNING || if_msg->ifi_flags & IFF_RUNNING)) {
			if_item_t* if_item = new_if_item();
			get_if_item_cb_data_t item_cb_data = new_get_if_item_cb_data(list_cb_data->compiled_regex, if_item);
			if_item->index = if_msg->ifi_index;
			if_item->type = if_msg->ifi_type;
			if_item->flags = if_msg->ifi_flags;
			if_item->family = if_msg->ifi_family;
			mnl_attr_parse(nl_head, sizeof(if_msg), &get_if_item_cb, &item_cb_data);
			destroy_get_if_item_cb_data(&item_cb_data);
			if (SHOW_BLACKLISTED || !if_item->blacklisted) {
				*list_cb_data->if_list = push_if_item(list_cb_data->if_list, if_item);
			} else {
				destroy_if_item(&if_item);
			}
		}
	
		return MNL_CB_OK;
	}
}

static int get_if_addr_list_cb(const struct nlmsghdr* nl_head, void* cb_data)
{
	if_list_t addr_list_cb_data = (if_list_t)cb_data;
	if (cb_data == NULL) {
		print_error("Invalid argument received in interface address list callback");
		return MNL_CB_ERROR;
	} else {
		struct ifaddrmsg* if_addr_msg = mnl_nlmsg_get_payload(nl_head);
		if_item_t* if_item = get_if_item_by_index(addr_list_cb_data, if_addr_msg->ifa_index);

		if (if_item != NULL && (SHOW_SECONDARY || !(if_addr_msg->ifa_flags & IFA_F_SECONDARY))) {
			if_addr_t* if_addr = new_if_addr();
			if_addr->family = if_addr_msg->ifa_family;
			if_addr->mask = if_addr_msg->ifa_prefixlen;
			if_addr->flags = if_addr_msg->ifa_flags;
			if_addr->scope = if_addr_msg->ifa_scope;
			mnl_attr_parse(nl_head, sizeof(if_addr_msg), &get_if_addr_cb, if_addr);
			if_item->addr_list = push_if_addr(&(if_item->addr_list), if_addr);
		}

		return MNL_CB_OK;
	}
}

static int print_neigh_list_cb(const struct nlmsghdr* nl_head, void* cb_data)
{
	print_neigh_list_cb_data_t* neigh_list_cb_data = (print_neigh_list_cb_data_t*)cb_data;
	if (neigh_list_cb_data == NULL || neigh_list_cb_data->fd == NULL) {
		print_error("Invalid argument received in neighbour list printing callback");
		return MNL_CB_ERROR;
	} else {
		struct ndmsg* nd_msg = mnl_nlmsg_get_payload(nl_head);
		if_item_t* if_item = get_if_item_by_index(*(neigh_list_cb_data->if_list), nd_msg->ndm_ifindex);

		if (if_item != NULL && (SHOW_BLACKLISTED_INTERFACE_NEIGHBOURS || !if_item->blacklisted)) {
			print_neigh_cb_data_t neigh_cb_data = new_print_neigh_cb_data(neigh_list_cb_data->fd, nd_msg->ndm_family);
			fprintf(neigh_list_cb_data->fd, ",{");

			if (nd_msg->ndm_state & NUD_INCOMPLETE) {
				fprintf(neigh_list_cb_data->fd, "\"incomplete\":1,");
			}
			if (nd_msg->ndm_state & NUD_REACHABLE) {
				fprintf(neigh_list_cb_data->fd, "\"reachable\":1,\"state\":\"reachable\",");
			} else {
				fprintf(neigh_list_cb_data->fd, "\"state\":\"unreachable\",");
			}
			if (nd_msg->ndm_state & NUD_STALE) {
				fprintf(neigh_list_cb_data->fd, "\"stale\":1,");
			}
			if (nd_msg->ndm_state & NUD_DELAY) {
				fprintf(neigh_list_cb_data->fd, "\"delay\":1,");
			}
			if (nd_msg->ndm_state & NUD_PROBE) {
				fprintf(neigh_list_cb_data->fd, "\"probe\":1,");
			}
			if (nd_msg->ndm_state & NUD_FAILED) {
				fprintf(neigh_list_cb_data->fd, "\"failed\":1,");
			}
			if (nd_msg->ndm_state & NUD_NOARP) {
				fprintf(neigh_list_cb_data->fd, "\"noarp\":1,");
			}
			if (nd_msg->ndm_state & NUD_PERMANENT) {
				fprintf(neigh_list_cb_data->fd, "\"permanent\":1,");
			}
			if (nd_msg->ndm_flags & NTF_PROXY) {
				fprintf(neigh_list_cb_data->fd, "\"proxy\":1,");
			}
			if (nd_msg->ndm_flags & NTF_ROUTER) {
				fprintf(neigh_list_cb_data->fd, "\"router\":1,");
			}
			mnl_attr_parse(nl_head, sizeof(nd_msg), &print_neigh_cb, &neigh_cb_data);
			destroy_print_neigh_cb_data(&neigh_cb_data);
			fprintf(neigh_list_cb_data->fd, "\"iface\":\"%s\"}", if_item->name);
		}

		return MNL_CB_OK;
	}
}












static if_list_t get_if_list(struct mnl_socket* nl_sock, regex_t* compiled_regex)
{
	char* buf;
	struct nlmsghdr* nl_head;
	struct rtgenmsg* rtnl_head;
	int errcode = 0;
	unsigned int seq;
	unsigned int portid;
	if_list_t if_list = new_if_list();
	get_if_list_cb_data_t get_if_list_cb_data = new_get_if_list_cb_data(compiled_regex, &if_list);
	buf = (char*)malloc(getpagesize());
	if (buf == NULL) {
		print_syserror("Unable to allocate memory for a netlink communication buffer");
		exit(EX_OSERR);
	}

	nl_head = mnl_nlmsg_put_header(buf);
	nl_head->nlmsg_type = RTM_GETLINK;
	nl_head->nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
	nl_head->nlmsg_seq = seq = time(NULL);
	rtnl_head = mnl_nlmsg_put_extra_header(nl_head, sizeof(struct rtgenmsg));
	rtnl_head->rtgen_family = AF_PACKET;

	portid = mnl_socket_get_portid(nl_sock);

	if (mnl_socket_sendto(nl_sock, nl_head, nl_head->nlmsg_len) < 0) {
		print_syserror("Unable to send interface list request to netlink");
		exit(EX_OSERR);
	}

	do {
		errcode = mnl_socket_recvfrom(nl_sock, buf, sizeof(buf));
	} while (errcode > 0 && (errcode = mnl_cb_run(buf, errcode, seq, portid, &get_if_list_cb, &get_if_list_cb_data)) <= MNL_CB_STOP);

	if (errcode == -1) {
		print_syserror("Unable to retrieve interface list from netlink");
		exit(EX_OSERR);
	}

	destroy_get_if_list_cb_data(&get_if_list_cb_data);
	free(buf);
	return if_list;
}

static void get_if_addrs(struct mnl_socket* nl_sock, if_list_t* if_list)
{
	char* buf;
	struct nlmsghdr* nl_head;
	struct rtgenmsg* rtnl_head;
	int errcode = 0;
	unsigned int seq;
	unsigned int portid;
	buf = (char*)malloc(getpagesize());
	if (buf == NULL) {
		print_syserror("Unable to allocate memory for a netlink communication buffer");
		exit(EX_OSERR);
	}

	nl_head = mnl_nlmsg_put_header(buf);
	nl_head->nlmsg_type = RTM_GETADDR;
	nl_head->nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
	nl_head->nlmsg_seq = seq = time(NULL);
	rtnl_head = mnl_nlmsg_put_extra_header(nl_head, sizeof(struct rtgenmsg));
	rtnl_head->rtgen_family = AF_PACKET;

	portid = mnl_socket_get_portid(nl_sock);

	if (mnl_socket_sendto(nl_sock, nl_head, nl_head->nlmsg_len) < 0) {
		print_syserror("Unable to send interface address list request to netlink");
		exit(EX_OSERR);
	}

	do {
		errcode = mnl_socket_recvfrom(nl_sock, buf, sizeof(buf));
	} while (errcode > 0 && (errcode = mnl_cb_run(buf, errcode, seq, portid, &get_if_addr_list_cb, if_list)) <= MNL_CB_STOP);

	if (errcode == -1) {
		print_syserror("Unable to retrieve interface address list from netlink");
		exit(EX_OSERR);
	}

	free(buf);
}

static void print_neigh_list(struct mnl_socket* nl_sock, FILE* fd, if_list_t if_list)
{
	char* buf;
	struct nlmsghdr* nl_head;
	struct rtgenmsg* rtnl_head;
	int errcode = 0;
	unsigned int seq;
	unsigned int portid;
	print_neigh_list_cb_data_t neigh_list_cb_data = new_print_neigh_list_cb_data(fd, &if_list);
	buf = (char*)malloc(getpagesize());
	if (buf == NULL) {
		print_syserror("Unable to allocate memory for a netlink communication buffer");
		exit(EX_OSERR);
	}

	nl_head = mnl_nlmsg_put_header(buf);
	nl_head->nlmsg_type = RTM_GETNEIGH;
	nl_head->nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
	nl_head->nlmsg_seq = seq = time(NULL);
	rtnl_head = mnl_nlmsg_put_extra_header(nl_head, sizeof(struct rtgenmsg));
	rtnl_head->rtgen_family = AF_PACKET;

	portid = mnl_socket_get_portid(nl_sock);

	if (mnl_socket_sendto(nl_sock, nl_head, nl_head->nlmsg_len) < 0) {
		print_syserror("Unable to send neighbour list request to netlink");
		exit(EX_OSERR);
	}

	do {
		errcode = mnl_socket_recvfrom(nl_sock, buf, sizeof(buf));
	} while (errcode > 0 && (errcode = mnl_cb_run(buf, errcode, seq, portid, &print_neigh_list_cb, &neigh_list_cb_data)) <= MNL_CB_STOP);
	destroy_print_neigh_list_cb_data(&neigh_list_cb_data);

	if (errcode == -1) {
		print_syserror("Unable to retrieve neighbour list from netlink");
		exit(EX_OSERR);
	}


	free(buf);
}

static void print_if_list(FILE* fd, if_list_t if_list)
{
	if_list_t if_list_iter = if_list;

	while (if_list_iter != NULL) {
		if_item_t* if_item = if_list_iter->data;
		if (!if_item->blacklisted) {
			if_addr_list_t if_addr_list_iter = if_item->addr_list;
			if_item_t* real_if_item = NULL;

			if (if_item->index != if_item->link) {
				real_if_item = get_if_item_by_index(if_list, if_item->link);
			}

			while (if_addr_list_iter != NULL) {
				char str_temp[BUFSIZ];
				if_addr_t* if_addr = if_addr_list_iter->data;
				fprintf(fd, ",{");

				if (if_item->flags & IFF_UP) {
					fprintf(fd, "\"ifup\":1,");
				}
				if (if_item->flags & IFF_BROADCAST) {
					fprintf(fd, "\"bcastset\":1,");
				}
				if (if_item->flags & IFF_POINTOPOINT) {
					fprintf(fd, "\"ptp\":1,");
				}
				if (if_item->flags & IFF_NOARP) {
					fprintf(fd, "\"noarp\":1,");
				}
				if (if_item->flags & IFF_PROMISC) {
					fprintf(fd, "\"promiscuous\":1,");
				}
				if (if_item->flags & IFF_NOTRAILERS) {
					fprintf(fd, "\"notrailers\":1,");
				}
				if (if_item->flags & IFF_ALLMULTI) {
					fprintf(fd, "\"recvallmcast\":1,");
				}
				if (if_item->flags & IFF_MASTER) {
					fprintf(fd, "\"lbmaster\":1,");
				}
				if (if_item->flags & IFF_SLAVE) {
					fprintf(fd, "\"lbslave\":1,");
				}
				if (if_item->flags & IFF_MULTICAST) {
					fprintf(fd, "\"supportmcast\":1,");
				}
				if (if_item->flags & IFF_PORTSEL) {
					fprintf(fd, "\"portsel\":1,");
				}
				if (if_item->flags & IFF_AUTOMEDIA) {
					fprintf(fd, "\"automedia\":1,");
				}
				if (if_item->flags & IFF_DYNAMIC) {
					fprintf(fd, "\"dynamic\":1,");
				}
				fprintf(fd, "\"iface\":\"%s\",", if_item->name);
				if (NULL == mac_ntop(if_item->mac, str_temp, 6)) {
					print_error("Unable to print MAC address for %s", if_item->name);
				} else {
					fprintf(fd, "\"macaddress\":\"%s\",", str_temp);
				}
				if (NULL == mac_ntop(if_item->bmac, str_temp, 6)) {
					print_error("Unable to print broadcast MAC address for %s", if_item->name);
				} else {
					fprintf(fd, "\"bmac\":\"%s\",", str_temp);
				}
				fprintf(fd, "\"mtu\":%"PRIu32",", if_item->mtu);
				if (real_if_item != NULL) {
					fprintf(fd, "\"realiface\":\"%s\",", real_if_item->name);
				}
				fprintf(fd, "\"qdiscipline\":\"%s\",", if_item->qdsp);
				if (if_addr->flags & IFA_F_SECONDARY) {
					fprintf(fd, "\"secondaryaddr\":1,");
				}
				if (if_addr->flags & IFA_F_PERMANENT) {
					fprintf(fd, "\"permanent\":1,");
				}
				fprintf(fd, "\"addrname\":\"%s\",", if_addr->label);
				fprintf(fd, "\"masklength\":%u,", if_addr->mask);
				if (if_addr->family == AF_INET) {
					if (NULL == inet_ntop(AF_INET, &(((struct sockaddr_in*)(if_addr->addr))->sin_addr.s_addr), str_temp, sizeof(struct in_addr))) {
						print_syserror("Unable to print an IP address for %s", if_item->name);
					} else {
						fprintf(fd, "\"ipaddress\":\"%s\",", str_temp);
					}
					if (NULL == inet_ntop(AF_INET, &(((struct sockaddr_in*)(if_addr->local))->sin_addr.s_addr), str_temp, sizeof(struct in_addr))) {
						print_syserror("Unable to print an internal IP address for %s", if_item->name);
					} else {
						fprintf(fd, "\"internalip\":\"%s\",", str_temp);
					}
					if (NULL == inet_ntop(AF_INET, &(((struct sockaddr_in*)(if_addr->bcast))->sin_addr.s_addr), str_temp, sizeof(struct in_addr))) {
						print_syserror("Unable to print a broadcast IP address for %s", if_item->name);
					} else {
						fprintf(fd, "\"bcast\":\"%s\",", str_temp);
					}
					if (NULL == inet_ntop(AF_INET, &(((struct sockaddr_in*)(if_addr->acast))->sin_addr.s_addr), str_temp, sizeof(struct in_addr))) {
						print_syserror("Unable to print an anycast IP address for %s", if_item->name);
					} else {
						fprintf(fd, "\"acast\":\"%s\",", str_temp);
					}
				} else if (if_addr->family == AF_INET6) {
					if (NULL == inet_ntop(AF_INET6, &(((struct sockaddr_in6*)(if_addr->addr))->sin6_addr.s6_addr), str_temp, sizeof(struct in6_addr))) {
						print_syserror("Unable to print an IP address for %s", if_item->name);
					} else {
						fprintf(fd, "\"ip6\":\"%s\",", str_temp);
					}
					if (NULL == inet_ntop(AF_INET6, &(((struct sockaddr_in6*)(if_addr->local))->sin6_addr.s6_addr), str_temp, sizeof(struct in6_addr))) {
						print_syserror("Unable to print an internal IP address for %s", if_item->name);
					} else {
						fprintf(fd, "\"internalip\":\"%s\",", str_temp);
					}
					if (NULL == inet_ntop(AF_INET6, &(((struct sockaddr_in6*)(if_addr->bcast))->sin6_addr.s6_addr), str_temp, sizeof(struct in6_addr))) {
						print_syserror("Unable to print a broadcast IP address for %s", if_item->name);
					} else {
						fprintf(fd, "\"bcast\":\"%s\",", str_temp);
					}
					if (NULL == inet_ntop(AF_INET6, &(((struct sockaddr_in6*)(if_addr->acast))->sin6_addr.s6_addr), str_temp, sizeof(struct in6_addr))) {
						print_syserror("Unable to print an anycast IP address for %s", if_item->name);
					} else {
						fprintf(fd, "\"acast\":\"%s\",", str_temp);
					}
				}
				fprintf(fd, "\"isagent\":1}");
				if_addr_list_iter = if_addr_list_iter->next;
			}
		}
		if_list_iter = if_list_iter->next;
	}
}





/*
static void scan_networks(struct mnl_socket* nl_sock, if_list_t* if_list, net_list_t* net_list)
{
}
*/

static void autoscan_networks(struct mnl_socket* nl_sock, if_list_t if_list)
{
	if_list_t if_item_iter = if_list;

	while (nl_sock != NULL && if_item_iter != NULL) {
		if_item_t* if_item = if_item_iter->data;

		if (!if_item->blacklisted) {
			if_addr_list_t if_addr_iter = if_item->addr_list;

			while (if_addr_iter != NULL) {
				if_addr_t* if_addr = if_addr_iter->data;

				if (check_scannable_range(if_addr->addr, if_addr->mask) > 0) {
					size_t remote_addr_size = ((if_addr->family == AF_INET))?sizeof(struct sockaddr_in):sizeof(struct sockaddr_in6);
					struct sockaddr* remote_addr = (struct sockaddr*)malloc(remote_addr_size);
					memset(remote_addr, 0, remote_addr_size);
					while (increment_addr(if_addr->addr, if_addr->mask, remote_addr) > 0) {
						int fdfl = 0;
						int sockfd = socket(AF_INET, SOCK_STREAM, 0);
						if (sockfd == -1) {
							print_syserror("Unable to create socket");
						} else if ((fdfl = fcntl(sockfd, F_GETFL)) == -1) {
							print_syserror("Unable to get socket metadata");
						} else if (fcntl(sockfd, F_SETFL, fdfl | O_NONBLOCK) == -1) {
							print_syserror("Unable to put socket into non-blocking mode");
						} else {
							char str_temp[BUFSIZ];
							int errcode = 0;
							if (remote_addr->sa_family == AF_INET) {
								struct sockaddr_in* remote_ip4 = (struct sockaddr_in*)remote_addr;
								if (NULL == inet_ntop(
										AF_INET,
										&remote_ip4->sin_addr.s_addr,
										str_temp,
										sizeof(struct sockaddr_in))) {
									print_syserror("Unable to parse IP address");
								}
								remote_ip4->sin_port = htons(9);
							} else {
								struct sockaddr_in6* remote_ip6 = (struct sockaddr_in6*)remote_addr;
								if (NULL == inet_ntop(
										AF_INET,
										&remote_ip6->sin6_addr.s6_addr,
										str_temp,
										sizeof(struct sockaddr_in6))) {
									print_syserror("Unable to parse IP address");
								}
								remote_ip6->sin6_port = htons(9);
							}
							errcode = connect(sockfd, remote_addr, remote_addr_size);
							if (errcode == -1) {
								if (errno != EINPROGRESS) {
									print_syserror("Unable to connect to %s", str_temp);
								}
							}
						}
						close(sockfd);
					}
				}

				if_addr_iter = if_addr_iter->next;
			}
		}

		if_item_iter = if_item_iter->next;
	}
}










void print_neighbours(config_t* config, FILE* fd)
{
	char uncompiled_regex[MAX_IFACE_BLACKLIST_REGEX_LENGTH + UNIVERSAL_IFACE_BLACKLIST_REGEX_LENGTH + 1];
	regex_t compiled_regex;
	if_list_t if_list;
	struct mnl_socket* nl_sock;
	int errcode = 0;

	if (config == NULL) {
		print_error("Empty config received");
		exit(EX_SOFTWARE);
	} else if (fd == NULL) {
		print_error("Bad file descriptor received");
	}

	if (strlen(config->str_iface_blacklist_regex) > 0) {
		snprintf(
				uncompiled_regex,
				MAX_IFACE_BLACKLIST_REGEX_LENGTH + UNIVERSAL_IFACE_BLACKLIST_REGEX_LENGTH + 1,
				UNIVERSAL_IFACE_BLACKLIST_REGEX "|%s",
				config->str_iface_blacklist_regex);
	} else {
		strcpy(uncompiled_regex, UNIVERSAL_IFACE_BLACKLIST_REGEX);
	}
	errcode = regcomp(&compiled_regex, uncompiled_regex, REG_EXTENDED | REG_ICASE);
	if (errcode != 0) {
		char str_temp[BUFSIZ];
		regerror(errcode, &compiled_regex, str_temp, BUFSIZ);
		print_error("Unable to prepare the network interface regex: %s", str_temp);
	}

	nl_sock = mnl_socket_open(NETLINK_ROUTE);
	if (nl_sock == NULL) {
		print_syserror("Unable to open netlink socket");
		exit(EX_OSERR);
	}

	if (mnl_socket_bind(nl_sock, 0, MNL_SOCKET_AUTOPID) < 0) {
		print_syserror("Unable to bind netlink socket to port");
		exit(EX_OSERR);
	}


	if_list = get_if_list(nl_sock, &compiled_regex);

	get_if_addrs(nl_sock, &if_list);

	/*
	if (config->network_list != NULL) {
		scan_networks(nl_sock, if_list, config->network_list);
	} else {
		autoscan_networks(nl_sock, if_list);
	}
	*/
	autoscan_networks(nl_sock, if_list);

	print_if_list(fd, if_list);

	sleep(30);

	print_neigh_list(nl_sock, fd, if_list);


	destroy_if_list(&if_list);
	mnl_socket_close(nl_sock);
}



