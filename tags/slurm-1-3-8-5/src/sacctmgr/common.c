/*****************************************************************************\
 *  common.c - definitions for functions common to all modules in sacctmgr.
 *****************************************************************************
 *  Copyright (C) 2008 Lawrence Livermore National Security.
 *  Copyright (C) 2002-2007 The Regents of the University of California.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Danny Auble <da@llnl.gov>
 *  LLNL-CODE-402394.
 *  
 *  This file is part of SLURM, a resource management program.
 *  For details, see <http://www.llnl.gov/linux/slurm/>.
 *  
 *  SLURM is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *
 *  In addition, as a special exception, the copyright holders give permission 
 *  to link the code of portions of this program with the OpenSSL library under
 *  certain conditions as described in each individual source file, and 
 *  distribute linked combinations including the two. You must obey the GNU 
 *  General Public License in all respects for all of the code used other than 
 *  OpenSSL. If you modify file(s) with this exception, you may extend this 
 *  exception to your version of the file(s), but you are not obligated to do 
 *  so. If you do not wish to do so, delete this exception statement from your
 *  version.  If you delete this exception statement from all source files in 
 *  the program, then also delete it here.
 *  
 *  SLURM is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *  
 *  You should have received a copy of the GNU General Public License along
 *  with SLURM; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
\*****************************************************************************/

#include "src/sacctmgr/sacctmgr.h"
#include <unistd.h>
#include <termios.h>

#define FORMAT_STRING_SIZE 32

static pthread_t lock_warning_thread;

static void *_print_lock_warn(void *no_data)
{
	sleep(5);
	printf(" Database is busy or waiting for lock from other user.\n");

	return NULL;
}

static void _nonblock(int state)
{
	struct termios ttystate;

	//get the terminal state
	tcgetattr(STDIN_FILENO, &ttystate);

	switch(state) {
	case 1:
		//turn off canonical mode
		ttystate.c_lflag &= ~ICANON;
		//minimum of number input read.
		ttystate.c_cc[VMIN] = 1;
		break;
	default:
		//turn on canonical mode
		ttystate.c_lflag |= ICANON;
	}
	//set the terminal attributes.
	tcsetattr(STDIN_FILENO, TCSANOW, &ttystate);

}

extern void destroy_sacctmgr_assoc(void *object)
{
	/* Most of this is pointers to something else that will be
	 * destroyed elsewhere.
	 */
	sacctmgr_assoc_t *sacctmgr_assoc = (sacctmgr_assoc_t *)object;
	if(sacctmgr_assoc) {
		if(sacctmgr_assoc->childern) {
			list_destroy(sacctmgr_assoc->childern);
		}
		xfree(sacctmgr_assoc);
	}
}

extern int parse_option_end(char *option)
{
	int end = 0;

	if(!option)
		return 0;

	while(option[end] && option[end] != '=')
		end++;

	if(!option[end])
		return 0;

	end++;
	return end;
}

/* you need to xfree whatever is sent from here */
extern char *strip_quotes(char *option, int *increased)
{
	int end = 0;
	int i=0, start=0;
	char *meat = NULL;
	char quote_c = '\0';
	int quote = 0;

	if(!option)
		return NULL;

	/* first strip off the ("|')'s */
	if (option[i] == '\"' || option[i] == '\'') {
		quote_c = option[i];
		quote = 1;
		i++;
	}
	start = i;

	while(option[i]) {
		if(quote && option[i] == quote_c) {
			end++;
			break;
		} else if(option[i] == '\"' || option[i] == '\'')
			option[i] = '`';
		else {
			char lower = tolower(option[i]);
			if(lower != option[i])
				option[i] = lower;
		}
		
		i++;
	}
	end += i;

	meat = xmalloc((i-start)+1);
	memcpy(meat, option+start, (i-start));

	if(increased)
		(*increased) += end;

	return meat;
}

extern int notice_thread_init()
{
	pthread_attr_t attr;
	
	slurm_attr_init(&attr);
	if(pthread_create(&lock_warning_thread, &attr, &_print_lock_warn, NULL))
		error ("pthread_create error %m");
	slurm_attr_destroy(&attr);
	return SLURM_SUCCESS;
}

extern int notice_thread_fini()
{
	return pthread_cancel(lock_warning_thread);
}

extern int commit_check(char *warning) 
{
	int ans = 0;
	char c = '\0';
	int fd = fileno(stdin);
	fd_set rfds;
	struct timeval tv;

	if(!rollback_flag)
		return 1;

	printf("%s (You have 30 seconds to decide)\n", warning);
	_nonblock(1);
	while(c != 'Y' && c != 'y'
	      && c != 'N' && c != 'n'
	      && c != '\n') {
		if(c) {
			printf("Y or N please\n");
		}
		printf("(N/y): ");
		fflush(stdout);
		FD_ZERO(&rfds);
		FD_SET(fd, &rfds);
		/* Wait up to 30 seconds. */
		tv.tv_sec = 30;
		tv.tv_usec = 0;
		if((ans = select(fd+1, &rfds, NULL, NULL, &tv)) <= 0)
			break;
		
		c = getchar();
		printf("\n");
	}
	_nonblock(0);
	if(ans <= 0) 
		printf("timeout\n");
	else if(c == 'Y' || c == 'y') 
		return 1;			
	
	return 0;
}

extern acct_association_rec_t *sacctmgr_find_association(char *user,
							 char *account,
							 char *cluster,
							 char *partition)
{
	acct_association_rec_t * assoc = NULL;
	acct_association_cond_t assoc_cond;
	List assoc_list = NULL;

	memset(&assoc_cond, 0, sizeof(acct_association_cond_t));
	if(account) {
		assoc_cond.acct_list = list_create(NULL);
		list_append(assoc_cond.acct_list, account);
	} else {
		error("need an account to find association");
		return NULL;
	}
	if(cluster) {
		assoc_cond.cluster_list = list_create(NULL);
		list_append(assoc_cond.cluster_list, cluster);
	} else {
		if(assoc_cond.acct_list)
			list_destroy(assoc_cond.acct_list);
		error("need an cluster to find association");
		return NULL;
	}

	assoc_cond.user_list = list_create(NULL);
	if(user) 
		list_append(assoc_cond.user_list, user);
	else
		list_append(assoc_cond.user_list, "");
	
	assoc_cond.partition_list = list_create(NULL);
	if(partition) 
		list_append(assoc_cond.partition_list, partition);
	else
		list_append(assoc_cond.partition_list, "");
	
	assoc_list = acct_storage_g_get_associations(db_conn, my_uid,
						     &assoc_cond);
	
	list_destroy(assoc_cond.acct_list);
	list_destroy(assoc_cond.cluster_list);
	list_destroy(assoc_cond.user_list);
	list_destroy(assoc_cond.partition_list);

	if(assoc_list)
		assoc = list_pop(assoc_list);

	list_destroy(assoc_list);
	
	return assoc;
}

extern acct_association_rec_t *sacctmgr_find_account_base_assoc(char *account,
								char *cluster)
{
	acct_association_rec_t *assoc = NULL;
	char *temp = "root";
	acct_association_cond_t assoc_cond;
	List assoc_list = NULL;

	if(!cluster)
		return NULL;

	if(account)
		temp = account;

	memset(&assoc_cond, 0, sizeof(acct_association_cond_t));
	assoc_cond.acct_list = list_create(NULL);
	list_append(assoc_cond.cluster_list, temp);
	assoc_cond.cluster_list = list_create(NULL);
	list_append(assoc_cond.cluster_list, cluster);
	assoc_cond.user_list = list_create(NULL);
	list_append(assoc_cond.user_list, "");

//	info("looking for %s %s in %d", account, cluster,
//	     list_count(sacctmgr_association_list));
	
	assoc_list = acct_storage_g_get_associations(db_conn, my_uid,
						     &assoc_cond);

	list_destroy(assoc_cond.acct_list);
	list_destroy(assoc_cond.cluster_list);
	list_destroy(assoc_cond.user_list);

	if(assoc_list)
		assoc = list_pop(assoc_list);

	list_destroy(assoc_list);

	return assoc;
}

extern acct_association_rec_t *sacctmgr_find_root_assoc(char *cluster)
{
	return sacctmgr_find_account_base_assoc(NULL, cluster);
}

extern acct_user_rec_t *sacctmgr_find_user(char *name)
{
	acct_user_rec_t *user = NULL;
	acct_user_cond_t user_cond;
	acct_association_cond_t assoc_cond;
	List user_list = NULL;
	
	if(!name)
		return NULL;
	
	memset(&user_cond, 0, sizeof(acct_user_cond_t));
	memset(&assoc_cond, 0, sizeof(acct_association_cond_t));
	assoc_cond.user_list = list_create(NULL);
	list_append(assoc_cond.user_list, name);
	user_cond.assoc_cond = &assoc_cond;

	user_list = acct_storage_g_get_users(db_conn, my_uid,
					     &user_cond);

	list_destroy(assoc_cond.user_list);

	if(user_list)
		user = list_pop(user_list);

	list_destroy(user_list);

	return user;
}

extern acct_account_rec_t *sacctmgr_find_account(char *name)
{
	acct_account_rec_t *account = NULL;
	acct_account_cond_t account_cond;
	acct_association_cond_t assoc_cond;
	List account_list = NULL;
	
	if(!name)
		return NULL;

	memset(&account_cond, 0, sizeof(acct_account_cond_t));
	memset(&assoc_cond, 0, sizeof(acct_association_cond_t));
	assoc_cond.acct_list = list_create(NULL);
	list_append(assoc_cond.acct_list, name);
	account_cond.assoc_cond = &assoc_cond;

	account_list = acct_storage_g_get_accounts(db_conn, my_uid,
						   &account_cond);
	
	list_destroy(assoc_cond.acct_list);

	if(account_list)
		account = list_pop(account_list);

	list_destroy(account_list);

	return account;
}

extern acct_cluster_rec_t *sacctmgr_find_cluster(char *name)
{
	acct_cluster_rec_t *cluster = NULL;
	acct_cluster_cond_t cluster_cond;
	List cluster_list = NULL;

	if(!name)
		return NULL;

	memset(&cluster_cond, 0, sizeof(acct_cluster_cond_t));
	cluster_cond.cluster_list = list_create(NULL);
	list_append(cluster_cond.cluster_list, name);

	cluster_list = acct_storage_g_get_clusters(db_conn, my_uid,
						   &cluster_cond);

	list_destroy(cluster_cond.cluster_list);

	if(cluster_list) 
		cluster = list_pop(cluster_list);

	list_destroy(cluster_list);
	
	return cluster;
}

extern acct_association_rec_t *sacctmgr_find_association_from_list(
	List assoc_list, char *user, char *account, 
	char *cluster, char *partition)
{
	ListIterator itr = NULL;
	acct_association_rec_t * assoc = NULL;
	
	if(!assoc_list)
		return NULL;
	
	itr = list_iterator_create(assoc_list);
	while((assoc = list_next(itr))) {
		if(((!user && assoc->user)
		    || (user && (!assoc->user
				 || strcasecmp(user, assoc->user))))
		   || ((!account && assoc->acct)
		       || (account && (!assoc->acct 
				       || strcasecmp(account, assoc->acct))))
		   || ((!cluster && assoc->cluster)
		       || (cluster && (!assoc->cluster 
				       || strcasecmp(cluster, assoc->cluster))))
		   || ((!partition && assoc->partition)
		       || (partition && (!assoc->partition 
					 || strcasecmp(partition, 
						       assoc->partition)))))
			continue;
		break;
	}
	list_iterator_destroy(itr);
	
	return assoc;
}

extern acct_association_rec_t *sacctmgr_find_account_base_assoc_from_list(
	List assoc_list, char *account, char *cluster)
{
	ListIterator itr = NULL;
	acct_association_rec_t *assoc = NULL;
	char *temp = "root";

	if(!cluster || !assoc_list)
		return NULL;

	if(account)
		temp = account;
	/* info("looking for %s %s in %d", account, cluster, */
/* 	     list_count(assoc_list)); */
	itr = list_iterator_create(assoc_list);
	while((assoc = list_next(itr))) {
		/* info("is it %s %s %s", assoc->user, assoc->acct, assoc->cluster); */
		if(assoc->user
		   || strcasecmp(temp, assoc->acct)
		   || strcasecmp(cluster, assoc->cluster))
			continue;
	/* 	info("found it"); */
		break;
	}
	list_iterator_destroy(itr);

	return assoc;
}

extern acct_qos_rec_t *sacctmgr_find_qos_from_list(
	List qos_list, char *name)
{
	ListIterator itr = NULL;
	acct_qos_rec_t *qos = NULL;
	
	if(!name || !qos_list)
		return NULL;
	
	itr = list_iterator_create(qos_list);
	while((qos = list_next(itr))) {
		if(!strcasecmp(name, qos->name))
			break;
	}
	list_iterator_destroy(itr);
	
	return qos;

}

extern acct_user_rec_t *sacctmgr_find_user_from_list(
	List user_list, char *name)
{
	ListIterator itr = NULL;
	acct_user_rec_t *user = NULL;
	
	if(!name || !user_list)
		return NULL;
	
	itr = list_iterator_create(user_list);
	while((user = list_next(itr))) {
		if(!strcasecmp(name, user->name))
			break;
	}
	list_iterator_destroy(itr);
	
	return user;

}

extern acct_account_rec_t *sacctmgr_find_account_from_list(
	List acct_list, char *name)
{
	ListIterator itr = NULL;
	acct_account_rec_t *account = NULL;
	
	if(!name || !acct_list)
		return NULL;

	itr = list_iterator_create(acct_list);
	while((account = list_next(itr))) {
		if(!strcasecmp(name, account->name))
			break;
	}
	list_iterator_destroy(itr);
	
	return account;

}

extern acct_cluster_rec_t *sacctmgr_find_cluster_from_list(
	List cluster_list, char *name)
{
	ListIterator itr = NULL;
	acct_cluster_rec_t *cluster = NULL;

	if(!name || !cluster_list)
		return NULL;

	itr = list_iterator_create(cluster_list);
	while((cluster = list_next(itr))) {
		if(!strcasecmp(name, cluster->name))
			break;
	}
	list_iterator_destroy(itr);
	
	return cluster;
}

extern int get_uint(char *in_value, uint32_t *out_value, char *type)
{
	char *ptr = NULL, *meat = NULL;
	long num;
	
	if(!(meat = strip_quotes(in_value, NULL)))
		return SLURM_ERROR;

	num = strtol(meat, &ptr, 10);
	if ((num == 0) && ptr && ptr[0]) {
		error("Invalid value for %s (%s)", type, meat);
		xfree(meat);
		return SLURM_ERROR;
	}
	xfree(meat);
	
	if (num < 0)
		*out_value = INFINITE;		/* flag to clear */
	else
		*out_value = (uint32_t) num;
	return SLURM_SUCCESS;
}

extern int addto_qos_char_list(List char_list, List qos_list, char *names, 
			       int option)
{
	int i=0, start=0;
	char *name = NULL, *tmp_char = NULL;
	ListIterator itr = NULL;
	char quote_c = '\0';
	int quote = 0;
	uint32_t id=0;
	int count = 0;

	if(!char_list) {
		error("No list was given to fill in");
		return 0;
	}

	if(!qos_list || !list_count(qos_list)) {
		debug2("No real qos_list");
		return 0;
	}

	itr = list_iterator_create(char_list);
	if(names) {
		if (names[i] == '\"' || names[i] == '\'') {
			quote_c = names[i];
			quote = 1;
			i++;
		}
		start = i;
		while(names[i]) {
			if(quote && names[i] == quote_c)
				break;
			else if (names[i] == '\"' || names[i] == '\'')
				names[i] = '`';
			else if(names[i] == ',') {
				if((i-start) > 0) {
					name = xmalloc((i-start+1));
					memcpy(name, names+start, (i-start));
					
					id = str_2_acct_qos(qos_list, name);
					xfree(name);
					if(id == NO_VAL) 
						goto bad;

					if(option) {
						name = xstrdup_printf(
							"%c%u", option, id);
					} else
						name = xstrdup_printf("%u", id);
					while((tmp_char = list_next(itr))) {
						if(!strcasecmp(tmp_char, name))
							break;
					}
					list_iterator_reset(itr);

					if(!tmp_char) {
						list_append(char_list, name);
						count++;
					} else 
						xfree(name);
				}
			bad:
				i++;
				start = i;
				if(!names[i]) {
					info("There is a problem with "
					     "your request.  It appears you "
					     "have spaces inside your list.");
					break;
				}
			}
			i++;
		}
		if((i-start) > 0) {
			name = xmalloc((i-start)+1);
			memcpy(name, names+start, (i-start));
			
			id = str_2_acct_qos(qos_list, name);
			xfree(name);
			if(id == NO_VAL) 
				goto end_it;
			
			if(option) {
				name = xstrdup_printf(
					"%c%u", option, id);
			} else
				name = xstrdup_printf("%u", id);
			while((tmp_char = list_next(itr))) {
				if(!strcasecmp(tmp_char, name))
					break;
			}
			
			if(!tmp_char) {
				list_append(char_list, name);
				count++;
			} else 
				xfree(name);
		}
	}	
end_it:
	list_iterator_destroy(itr);
	return count;
} 

extern void sacctmgr_print_coord_list(
	print_field_t *field, List value, int last)
{
	ListIterator itr = NULL;
	char *print_this = NULL;
	acct_coord_rec_t *object = NULL;
	
	if(!value || !list_count(value)) {
		if(print_fields_parsable_print)
			print_this = xstrdup("");
		else
			print_this = xstrdup(" ");
	} else {
		list_sort(value, (ListCmpF)sort_coord_list);
		itr = list_iterator_create(value);
		while((object = list_next(itr))) {
			if(print_this) 
				xstrfmtcat(print_this, ",%s", 
					   object->name);
			else 
				print_this = xstrdup(object->name);
		}
		list_iterator_destroy(itr);
	}
	
	if(print_fields_parsable_print == PRINT_FIELDS_PARSABLE_NO_ENDING
	   && last)
		printf("%s", print_this);
	else if(print_fields_parsable_print)
		printf("%s|", print_this);
	else {
		if(strlen(print_this) > field->len) 
			print_this[field->len-1] = '+';
		
		printf("%-*.*s ", field->len, field->len, print_this);
	}
	xfree(print_this);
}

extern void sacctmgr_print_qos_list(print_field_t *field, List qos_list,
				    List value, int last)
{
	char *print_this = NULL;

	print_this = get_qos_complete_str(qos_list, value);
	
	if(print_fields_parsable_print == PRINT_FIELDS_PARSABLE_NO_ENDING
	   && last)
		printf("%s", print_this);
	else if(print_fields_parsable_print)
		printf("%s|", print_this);
	else {
		if(strlen(print_this) > field->len) 
			print_this[field->len-1] = '+';
		
		printf("%-*.*s ", field->len, field->len, print_this);
	}
	xfree(print_this);
}

extern char *get_qos_complete_str(List qos_list, List num_qos_list)
{
	List temp_list = NULL;
	char *temp_char = NULL;
	char *print_this = NULL;
	ListIterator itr = NULL;

	if(!qos_list || !list_count(qos_list)
	   || !num_qos_list || !list_count(num_qos_list))
		return xstrdup("");

	temp_list = list_create(NULL);

	itr = list_iterator_create(num_qos_list);
	while((temp_char = list_next(itr))) {
		temp_char = acct_qos_str(qos_list, atoi(temp_char));
		if(temp_char)
			list_append(temp_list, temp_char);
	}
	list_iterator_destroy(itr);
	list_sort(temp_list, (ListCmpF)sort_char_list);
	itr = list_iterator_create(temp_list);
	while((temp_char = list_next(itr))) {
		if(print_this) 
			xstrfmtcat(print_this, ",%s", temp_char);
		else 
			print_this = xstrdup(temp_char);
	}
	list_iterator_destroy(itr);
	list_destroy(temp_list);

	if(!print_this)
		return xstrdup("");

	return print_this;
}

extern int sort_coord_list(acct_coord_rec_t *coord_a, acct_coord_rec_t *coord_b)
{
	int diff = strcmp(coord_a->name, coord_b->name);

	if (diff < 0)
		return -1;
	else if (diff > 0)
		return 1;
	
	return 0;
}

extern int sort_char_list(char *name_a, char *name_b)
{
	int diff = strcmp(name_a, name_b);

	if (diff < 0)
		return -1;
	else if (diff > 0)
		return 1;
	
	return 0;
}
