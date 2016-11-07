//////////////////////////////////////////////////////////////////////
//                             North Carolina State University
//
//
//
//                             Copyright 2016
//
////////////////////////////////////////////////////////////////////////
//
// This program is free software; you can redistribute it and/or modify it
// under the terms and conditions of the GNU General Public License,
// version 2, as published by the Free Software Foundation.
//
// This program is distributed in the hope it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
// FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
// more details.
//
// You should have received a copy of the GNU General Public License along with
// this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
//
////////////////////////////////////////////////////////////////////////
//
//   Authors:  Hung-Wei Tseng
//				Abhinav Choudhury (Student ID: 200159347)
//				Aditya Virmani	(Student ID: 200068593)
//
//   Description:
//     Skeleton of KeyValue Pseudo Device
//
////////////////////////////////////////////////////////////////////////

#include "keyvalue.h"

#include <asm/uaccess.h>
#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/poll.h>
#include <linux/string.h>
#include <linux/semaphore.h>
#include <linux/mutex.h>

#define HASHTABLESIZE (1024)
#define HASHKEYMASK (0x3ff)
#define TOP54MASK (0xfffffffffffffc00)
#define MAX_READERS (10)

/*
	We use a hash-table with Linked Lists for conflict resolution.
	Currently, the size of the hash-table is given by HASHTABLESIZE which is 1024
	The hash-function used is a bit-mask for the lower 10 bits of the key (given by HASHKEYMASK)
	TOP54MASK is an octal mask for the remaining upper 54 bits of the 64-bit key
	We limit the maximum number of readers that can simultaneously read from the key-value store to MAX_READERS = 10

	LOCKING MECHANISM:
	We use a single mutex "transaction_lock" to synchronize updates to transaction_id
	The "update_lock" is a global write-lock
	Using the write lock we have a reader lock "r_lock"

	The rest, as they say, is history. Good luck!
*/

volatile unsigned transaction_id;
typedef struct mutex mutex_t;
mutex_t transaction_lock;
mutex_t update_lock;

struct read_lock {
	mutex_t read_sem;
	int readers;
} r_lock;

struct hash_node {
	u64 top54_bits;
	void* data;
	u64 datasize;
	struct hash_node* next;
};

struct hash_node* KV_HashTable[HASHTABLESIZE];

static void acquire_read_lock(struct read_lock* r_lock)
{
	mutex_lock(&(r_lock->read_sem));
	r_lock->readers += 1;
	if (r_lock->readers == 1)
		mutex_lock(&update_lock);
	mutex_unlock(&(r_lock->read_sem));
}

static void release_read_lock(struct read_lock* r_lock)
{
	mutex_lock(&(r_lock->read_sem));
	r_lock->readers -= 1;
	if (r_lock->readers == 0)
		mutex_unlock(&update_lock);
	mutex_unlock(&(r_lock->read_sem));
}

static void free_callback(void *data)
{
}

static long keyvalue_get(struct keyvalue_get __user *ukv)
{
    struct keyvalue_get kv;
	u64 hashkey = (ukv->key & HASHKEYMASK);
	u64 topbits = ((ukv->key & TOP54MASK) >> 10);	
	if (KV_HashTable[hashkey] == NULL)
	{
		return -1;
	}
	else
	{
		acquire_read_lock(&r_lock);
		struct hash_node* iter = KV_HashTable[hashkey];

		while (iter->top54_bits != topbits && iter != NULL)
			iter = iter->next;

		if (iter != NULL)	// We have found a key-value pair corresponding to the given key
		{
			*(ukv->size) = iter->datasize;
			memcpy((char*)(ukv->data), (char*)(iter->data), iter->datasize);
		}
		else	// No key-value entry was found
		{
			release_read_lock(&r_lock);
			return -1;
		}

		release_read_lock(&r_lock);
	}

	mutex_lock(&transaction_lock);
	transaction_id++;
	mutex_unlock(&transaction_lock);

    return transaction_id;
}

static long keyvalue_set(struct keyvalue_set __user *ukv)
{
    struct keyvalue_set kv;
	u64 hashkey = (ukv->key & HASHKEYMASK);
	u64 topbits = ((ukv->key & TOP54MASK) >> 10);

	mutex_lock(&update_lock);

	if (KV_HashTable[hashkey] == NULL)	// There is nothing there for this key. Create a new entry
	{
		KV_HashTable[hashkey] = kmalloc(sizeof(struct hash_node), GFP_KERNEL);
		if (KV_HashTable[hashkey] == NULL)
		{
			printk("Kernel allocation failure!\n");	
			mutex_unlock(&update_lock);
			return -1;
		}
		(KV_HashTable[hashkey])->top54_bits = topbits;
		(KV_HashTable[hashkey])->datasize = ukv->size;
		(KV_HashTable[hashkey])->data = kmalloc(ukv->size, GFP_KERNEL);
		if (KV_HashTable[hashkey]->data == NULL)
		{
			printk(KERN_ERR "Kernel allocation failure!\n");
			kfree(KV_HashTable[hashkey]);
			mutex_unlock(&update_lock);
			return -1;
		}
		(KV_HashTable[hashkey])->next = NULL;
		memcpy((char*)(KV_HashTable[hashkey])->data, (char*)(ukv->data), ukv->size);
	}
	else
	{
		// First check if a key-value pair for the given key already exists
		// If so, update it
		struct hash_node* last = KV_HashTable[hashkey];
		while (last->top54_bits != topbits && last->next != NULL)
			last = last->next;

		if (last->top54_bits == topbits) // We already have an entry
		{
			last->datasize = ukv->size;
			void* old_data = last->data;
			last->data = kmalloc(ukv->size, GFP_KERNEL);
			if (last->data == NULL)
			{
				printk(KERN_ERR "Kernel allocation failure!\n");
				last->data = old_data;
				mutex_unlock(&update_lock);
				return -1;				
			}
			memcpy((char*)(last->data), (char*)(ukv->data), ukv->size);
			kfree(old_data);
		}
		else if (last->next == NULL)	// Nah, we didn't find a match. Create a new entry
		{
			last->next = kmalloc(sizeof(struct hash_node), GFP_KERNEL);
			if (last->next == NULL)
			{
				printk(KERN_ERR "Kernel allocation failure!\n");
				mutex_unlock(&update_lock);
				return -1;
			}
			struct hash_node* prev = last;
			last = last->next;
	
			last->top54_bits = topbits;
			last->datasize = ukv->size;
			last->next = NULL;
			last->data = kmalloc(ukv->size, GFP_KERNEL);
			if (last->data == NULL)
			{
				printk(KERN_ERR "Kernel allocation failure!\n");
				kfree(last);
				prev->next = NULL;
				mutex_unlock(&update_lock);
				return -1;
			}
			memcpy((char*)(last->data), (char*)(ukv->data), ukv->size);
		}
		else	// This is not supposed to happen!
		{
			mutex_unlock(&update_lock);
			printk(KERN_ERR "Oops! Not supposed to happen...!\n");
			return -1;
		}
	}

	mutex_unlock(&update_lock);

	mutex_lock(&transaction_lock);
	transaction_id++;
	mutex_unlock(&transaction_lock);
	
    return transaction_id;
}

static long keyvalue_delete(struct keyvalue_delete __user *ukv)
{
    struct keyvalue_delete kv;
	u64 hashkey = (ukv->key & HASHKEYMASK);
	u64 topbits = ((ukv->key & TOP54MASK) >> 10);

	mutex_lock(&update_lock);

	if (KV_HashTable[hashkey] == NULL)	// No such entry for key
	{
		mutex_unlock(&update_lock);
		return -1;
	}
	else
	{
		struct hash_node* last = KV_HashTable[hashkey];
		while (last->top54_bits != topbits && last->next != NULL)
			last = last->next;
		
		if (last->top54_bits == topbits) // We already have an entry
		{
			// Remove the node from the Hash Table linked list
			struct hash_node* prev = KV_HashTable[hashkey];

			if (prev == last)	// Entry is at head of LL
			{
				// First free the memory for the value data
				kfree(prev->data);

				// Now free the linked list node
				KV_HashTable[hashkey] = prev->next;
				kfree(prev);
				prev = last = NULL;
			}
			else	// Entry is somewhere else in the LL
			{
				while (prev->next != last)
					prev = prev->next;

				// Free the memory for value data in this node
				kfree(last->data);

				// Unhook this node from LL
				prev->next = last->next;
				last->next = NULL;
				kfree(last);
				last = NULL;
			}
		}
		else if (last->next == NULL)
		{
			mutex_unlock(&update_lock);
			return -1;
		}
		else	// This is not supposed to happen!
		{
			mutex_unlock(&update_lock);
			printk(KERN_ERR "Oops! Not supposed to happen...!\n");
			return -1;
		}
	}

	mutex_unlock(&update_lock);

	mutex_lock(&transaction_lock);
	transaction_id++;
	mutex_unlock(&transaction_lock);

    return transaction_id;
}

//Added by Hung-Wei
     
unsigned int keyvalue_poll(struct file *filp, struct poll_table_struct *wait)
{
    unsigned int mask = 0;
    printk("keyvalue_poll called. Process queued\n");
    return mask;
}

static long keyvalue_ioctl(struct file *filp, unsigned int cmd,
                                unsigned long arg)
{
    switch (cmd) {
    case KEYVALUE_IOCTL_GET:
        return keyvalue_get((void __user *) arg);
    case KEYVALUE_IOCTL_SET:
        return keyvalue_set((void __user *) arg);
    case KEYVALUE_IOCTL_DELETE:
        return keyvalue_delete((void __user *) arg);
    default:
        return -ENOTTY;
    }
}

static int keyvalue_mmap(struct file *filp, struct vm_area_struct *vma)
{
    return 0;
}

static const struct file_operations keyvalue_fops = {
    .owner                = THIS_MODULE,
    .unlocked_ioctl       = keyvalue_ioctl,
    .mmap                 = keyvalue_mmap,
//    .poll		  = keyvalue_poll,
};

static struct miscdevice keyvalue_dev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "keyvalue",
    .fops = &keyvalue_fops,
};

static void initialize_locks(void)
{
	mutex_init(&transaction_lock);

	r_lock.readers = 0;

	mutex_init(&r_lock.read_sem);
	mutex_init(&update_lock);
}

static int __init keyvalue_init(void)
{
    int ret;

    if ((ret = misc_register(&keyvalue_dev)))
        printk(KERN_ERR "Unable to register \"keyvalue\" misc device\n");

    initialize_locks();
    return ret;
}

static void __exit keyvalue_exit(void)
{
    misc_deregister(&keyvalue_dev);
}

MODULE_AUTHOR("Hung-Wei Tseng <htseng3@ncsu.edu>");
MODULE_LICENSE("GPL");
MODULE_VERSION("0.1");
module_init(keyvalue_init);
module_exit(keyvalue_exit);
