//
// Created by Ziv on 27/05/2022.
//

#include <math.h>
#include <threads.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <sys/types.h>
#include <dirent.h>
#include <linux/limits.h>
#include <sys/stat.h>
#include <unistd.h>

#define ALLOCATION_ERROR
#define COND_INIT_ERROR {perror("ERROR! COND_INIT failed"); exit(-1);}
#define MTX_INIT_ERROR {perror("ERROR! COND_INIT failed"); exit(-1);}


typedef struct fifo{
    int size;
    struct dir_node *head;
    struct dir_node *tail;
}fifo;

typedef struct dir_node{
    char full_path[PATH_MAX];
    DIR *dirp;
    struct dir_node *next;
}dir_node;

// global variables
atomic_int  num_of_matched_files;
fifo *directories_fifo;
int thread_exit_with_error;
char *term;
atomic_int num_of_waiting_or_exited_threads;
int num_of_threads;
atomic_int created_threads;
cnd_t start_cond;
cnd_t fifo_has_directories_cond;
cnd_t threads_created_cond;
cnd_t directories_fifo_mutex_free;
mtx_t directories_fifo_mutex;
mtx_t start_mutex;


void print_fifo(fifo *fifo){
    dir_node *current = fifo -> head;
    printf("fifo is:\n");
    while(current!=NULL){
        printf("%s\n", current->full_path);
        current = current->next;
    }
}

void creat_full_path(char full_path[], char* dir_full_path, char* filename){
    strcpy(full_path, dir_full_path);
    strcat(full_path, "/");
    strcat(full_path, filename);
}
void add_directory_to_fifo(DIR *dirp, char* full_path){
    dir_node *node = malloc(sizeof(dir_node));
    if(!node) {perror("ERROR! memory allocation failed"); thrd_exit(thrd_error);}
    strcpy(node -> full_path, full_path);
    node -> dirp = dirp;
    node -> next = NULL;
    while(mtx_trylock(&directories_fifo_mutex)==thrd_busy)
        cnd_wait(&directories_fifo_mutex_free, NULL);
    directories_fifo -> size += 1;
    if(directories_fifo -> size == 1)
        directories_fifo -> head = node;
    else
        directories_fifo -> tail -> next = node;
    directories_fifo -> tail = node;
    mtx_unlock(&directories_fifo_mutex);
    cnd_broadcast(&fifo_has_directories_cond);
    cnd_broadcast(&directories_fifo_mutex_free); // cnd_signal?
}

// assumption - fifo isn't empty
dir_node *pop_directory_from_fifo(){
    dir_node *result = directories_fifo->head;
    directories_fifo -> size -= 1;
    directories_fifo -> head =  result -> next;
    if(directories_fifo -> size == 0)
        directories_fifo -> tail = NULL;
    result -> next = NULL;
    return result;
}

int thread_flow(void *t){
    mtx_lock(&start_mutex);
    created_threads ++;
    if(created_threads == num_of_threads)
        cnd_broadcast(&threads_created_cond);
    cnd_wait(&start_cond, &start_mutex);
    mtx_unlock(&start_mutex);

    while(1) {
        while(mtx_trylock(&directories_fifo_mutex)==thrd_busy)
            cnd_wait(&directories_fifo_mutex_free, NULL);


        while (directories_fifo->size == 0) {
            num_of_waiting_or_exited_threads += 1;
            if (num_of_waiting_or_exited_threads >= num_of_threads) {
                mtx_unlock(&directories_fifo_mutex);
                cnd_signal(&directories_fifo_mutex_free);
                cnd_broadcast(&fifo_has_directories_cond);
                thrd_exit(thrd_success);
            }
            cnd_wait(&fifo_has_directories_cond, &directories_fifo_mutex);
            num_of_waiting_or_exited_threads -= 1;
        }

        dir_node *node = pop_directory_from_fifo();
        mtx_unlock(&directories_fifo_mutex);
        cnd_broadcast(&directories_fifo_mutex_free); // cnd_signal?

        struct dirent *file = readdir(node->dirp);
        while (file != NULL) {
            if (strcmp(file->d_name, ".")!=0 && strcmp(file->d_name, "..")!=0) {
                char full_path[PATH_MAX];
                creat_full_path(full_path, node->full_path, file->d_name);
                struct stat buff;
                int result = stat(full_path, &buff);
                if (result != 0) {
                    perror("ERROR! stat failed.\n");
                    num_of_waiting_or_exited_threads += 1;
                    thrd_exit(thrd_error);
                }
                if (S_ISDIR(buff.st_mode)) {
                    DIR *dirp = opendir(full_path);
                    if (dirp == NULL)
                        printf("Directory %s: Permission denied.\n", full_path);
                    else {
                        add_directory_to_fifo(dirp, full_path);
                        cnd_signal(&fifo_has_directories_cond);
                    }
                } else if (strstr(file->d_name, term)) {
                    num_of_matched_files++;
                    printf("%s\n", full_path);
                }
            }
            file = readdir(node->dirp);
        }

        closedir(node->dirp);
        free(node);
    }
    thrd_exit(thrd_success);
}



int main(int argc, char *argv[]) {

    //initialization
    num_of_matched_files = 0;
    thread_exit_with_error = 0;
    created_threads = 0;

    // get the data
    if (argc < 4) { perror("ERROR! wrong command line parameters number"); exit(1);}
    num_of_threads = atoi(argv[3]);
    if (num_of_threads <= 0) {perror("ERROR! wrong number of threads"); exit(1);}
    term = argv[2];

    // Initialize mutex and condition variable objects
    if(cnd_init(&start_cond)!=thrd_success) COND_INIT_ERROR
    if(cnd_init(&fifo_has_directories_cond)!=thrd_success) COND_INIT_ERROR
    if(cnd_init(&threads_created_cond)!=thrd_success) COND_INIT_ERROR
    if(cnd_init(&directories_fifo_mutex_free)!=thrd_success) COND_INIT_ERROR
    if(mtx_init(&directories_fifo_mutex, mtx_plain)!=thrd_success) MTX_INIT_ERROR
    if(mtx_init(&start_mutex, mtx_plain)!=thrd_success) MTX_INIT_ERROR

    // create the directories fifo
    directories_fifo = malloc(sizeof(fifo));
    if (!directories_fifo) {perror("ERROR! memory allocation failed"); exit(1);}
    DIR *dirp = opendir(argv[1]);
    if(dirp == NULL) {printf("ERROR! Directory %s: Permission denied.\n", argv[1]); exit(1);}
    add_directory_to_fifo(dirp, argv[1]);


    // create the threads
    thrd_t threads_array[num_of_threads]; // thrd_t
    mtx_lock(&start_mutex);
    for (int i = 0; i < num_of_threads; i++) {
        if (thrd_create(&threads_array[i], thread_flow, NULL) != thrd_success) {
            perror("ERROR! thread creation failed\n");
            exit(1);
        }
    }

    // signal the threads to start
    cnd_wait(&threads_created_cond, &start_mutex);
    cnd_broadcast(&start_cond);
    mtx_unlock(&start_mutex);

    // Wait for all threads to complete
    for (int i = 0; i < num_of_threads; i++)
        if(thrd_join(threads_array[i], NULL)!=thrd_success)
            thread_exit_with_error = 1;

    // print the sum of founded files
    printf("Done searching, found %d files\n", num_of_matched_files);

    // frees
    free(directories_fifo);
    cnd_destroy(&start_cond);
    cnd_destroy(&fifo_has_directories_cond);
    cnd_destroy(&threads_created_cond);
    cnd_destroy(&directories_fifo_mutex_free);
    mtx_destroy(&directories_fifo_mutex);
    mtx_destroy(&start_mutex);

    exit(thread_exit_with_error);
}




