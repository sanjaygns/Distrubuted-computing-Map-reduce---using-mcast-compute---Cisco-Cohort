#include <stdio.h>
#include <stdlib.h>
#include <netdb.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <errno.h>
#include <arpa/inet.h>
#include <pthread.h>

#define SERVER_PORT atoi(argv[2])
#define SERVER_ADDR (inet_addr(argv[1]))
#define MAX_CONNECTIONS 100
#define OLD_NODE        0
#define NEW_NODE        1
#define TRUE            1
#define FALSE           0

typedef struct client_details_t_
{
    char ip_add[16];
    int  port;
    int  sock;
    char msg[1][100];
    struct client_details_t_ *next;
} client_detail_t;

typedef struct thread_args_t_ {
    int sock;
    struct sockaddr_in cli_addr;
} thread_args_t;

static client_detail_t *client_info[MAX_CONNECTIONS];

void doprocessing (void *thread_args);
void *read_periodic_client_msg (void *args);
void update_client_info(int group_no, int sock, 
                        char *buffer, int buffer_len, char *ip_addr, int port);
void update_new_message_from_client(int sock, char *buffer, 
                int buffer_len, char *ip_addr, int port);

pthread_t tid;

static int
make_socket_non_blocking (int sfd)
{
  int flags, s;

  flags = fcntl (sfd, F_GETFL, 0);
  if (flags == -1)
    {
      perror ("fcntl");
      return -1;
    }

  flags |= O_NONBLOCK;
  s = fcntl (sfd, F_SETFL, flags);
  if (s == -1)
    {
      perror ("fcntl");
      return -1;
    }

  return 0;
}

int main( int argc, char *argv[] ) 
{
    int sockfd, newsockfd, portno, clilen, rc;
    char buffer[256];
    struct sockaddr_in serv_addr, cli_addr;
    int n, pid;
    int epoll_fd;
    struct epoll_event event;
    struct epoll_event *events;
    thread_args_t *thread_args;

    memset(&client_info[0], 0, MAX_CONNECTIONS*sizeof(int));

    /* First call to socket() function */
    sockfd = socket(AF_INET, SOCK_STREAM, 0);

    if (sockfd < 0) {
        perror("ERROR opening socket");
        exit(1);
    }

    /* Create thread for periodic message */
    if( pthread_create(&tid , NULL ,  &read_periodic_client_msg , NULL) < 0)
    {
        perror("could not create thread");
        return 1;
    }

    /* Initialize socket structure */
    bzero((char *) &serv_addr, sizeof(serv_addr));
    portno = SERVER_PORT;

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = SERVER_ADDR;
    serv_addr.sin_port = htons(portno);

    /* Now bind the host address using bind() call.*/
    if (bind(sockfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0) {
        perror("ERROR on binding");
        exit(1);
    }

    /* 
     * Now start listening for the clients, here
     * process will go in sleep mode and will wait
     * for the incoming connection
     */

    listen(sockfd, MAX_CONNECTIONS);
    clilen = sizeof(cli_addr);

    rc = make_socket_non_blocking (sockfd);
    if (rc == -1) {
        perror("ERROR on making sock non blocking");
        exit(1);
    }

    epoll_fd = epoll_create1 (0);
    if (epoll_fd == -1)
    {
        perror ("epoll_create");
        exit(1);
    }

    event.data.fd = sockfd;
    event.events = EPOLLIN | EPOLLET | EPOLLOUT;
    rc = epoll_ctl (epoll_fd, EPOLL_CTL_ADD, sockfd, &event);
    if (rc == -1)
    {
        perror ("epoll_ctl");
        exit (1);
    }

    /* Buffer where events are returned */
    events = calloc (MAX_CONNECTIONS, sizeof event);

    printf("Listening on %s : %d \n", argv[1], SERVER_PORT);


    while (1)
    {
        int n, i;

        n = epoll_wait (epoll_fd, events, MAX_CONNECTIONS, -1);
        for (i = 0; i < n; i++)
        {
            if ((events[i].events & EPOLLERR) ||
                (events[i].events & EPOLLHUP) ||
                (!(events[i].events & EPOLLIN)))
            {
                /* An error has occured on this fd, or the socket is not
                   ready for reading (why were we notified then?) */
                fprintf (stderr, "epoll error\n");
                close (events[i].data.fd);
                continue;
            }

            else if (sockfd == events[i].data.fd)
            {
                /* We have a notification on the listening socket, which
                   means one or more incoming connections. */
                while (1)
                {
                    int infd;

                    infd = accept (sockfd, (struct sockaddr *) &cli_addr, (socklen_t *)&clilen);
                    if (infd == -1)
                    {
                        if ((errno == EAGAIN) ||
                                (errno == EWOULDBLOCK))
                        {
                            /* We have processed all incoming
                               connections. */
                            break;
                        }
                        else
                        {
                            perror ("accept");
                            break;
                        }
                    }

                    /* Make the incoming socket non-blocking and add it to the
                       list of fds to monitor. */
                    rc = make_socket_non_blocking (infd);
                    if (rc == -1)
                        exit (1);

                    thread_args = (thread_args_t *)malloc(sizeof(thread_args_t));
                    thread_args->sock = infd;
                    memcpy(&thread_args->cli_addr, &cli_addr, sizeof(struct sockaddr_in));

                    event.data.fd = infd;
                    event.events = EPOLLIN | EPOLLET;
                    event.data.ptr = (void *) thread_args;
                    rc = epoll_ctl (epoll_fd, EPOLL_CTL_ADD, infd, &event);
                    if (rc == -1)
                    {
                        perror ("epoll_ctl");
                        exit (i);
                    }

                }
                continue;
            }
            else
            {
                    doprocessing(events[i].data.ptr);
            }
        }
    }
}

void doprocessing (void *thread_args) {
    int n;
    char buffer[256];
    char quit[]="Quit\n";
    
    int group_no;
    int sock = ((thread_args_t *)thread_args)->sock;
    struct sockaddr_in * cli_addr = &((thread_args_t *)thread_args)->cli_addr;
    
   //strcpy(quit,"Quit");

  //  while(1) {
        group_no = 0;
        bzero(buffer,256);
        n = read(sock,buffer,255);

        if (n < 0) {
            perror("ERROR reading from socket");
            exit(1);
        }

        if(strcmp(buffer,quit) == 0) {
            printf(" Connection CLosed for %s:%d \n",
                    inet_ntoa(cli_addr->sin_addr),ntohs(cli_addr->sin_port));
            update_new_message_from_client(sock, "Connection Closed", 18, 
                    inet_ntoa(cli_addr->sin_addr),ntohs(cli_addr->sin_port));
            close(sock);
         //   break;
        }

        if(atoi(buffer)) {
            group_no = atoi(buffer);
            memset(buffer, 0, n);
            printf("The Group Num is %d \n", group_no);
        } else {
            printf("The Message is %s \n", buffer);
        }

        printf(" From %s:%d \n",
                inet_ntoa(cli_addr->sin_addr),ntohs(cli_addr->sin_port));
 

        if(group_no) {
            update_client_info(group_no, sock, buffer, n, 
                    inet_ntoa(cli_addr->sin_addr),ntohs(cli_addr->sin_port));
        } else {
            update_new_message_from_client(sock, buffer, n, 
                        inet_ntoa(cli_addr->sin_addr),ntohs(cli_addr->sin_port));
        }

        n = write(sock,"I got your message",18);

        if (n < 0) {
            perror("ERROR writing to socket");
            exit(1);
        }
//    }
  return;
}

void update_client_info(int group_no, int sock, 
        char *buffer, int buffer_len, char *ip_addr, int port)
{
    client_detail_t *temp;
    client_detail_t *new_node;
    int flag = NEW_NODE;

    printf("In Update Client Info \n");

    new_node = (client_detail_t *)malloc(sizeof(client_detail_t));

    memcpy(new_node->ip_add, ip_addr, sizeof(new_node->ip_add));
    new_node->port = port;
    memcpy(new_node->msg, buffer, buffer_len);
    new_node->sock = sock;

    printf("PK %s : %d : Msg : %s\n", new_node->ip_add, new_node->port, new_node->msg[0]);

    if (client_info[group_no] == 0)
    {
        client_info[group_no] = new_node;
        new_node->next=NULL;
    }
    else
    {
        /* If same IP and PORT exist, don't add a new entry */
        temp = client_info[group_no];
        while(temp) {
            if((temp->port == new_node->port) && 
                    (temp->sock == new_node->sock) &&
                    (strcmp(temp->ip_add, new_node->ip_add)==0)) {
                flag = OLD_NODE;
                printf("\n ***** OLD Node ******* \n");
                break;
            }

            if(temp->next == NULL) {
                break;
            } else {
                temp = temp->next;
            }
        }

        if(flag == OLD_NODE)
        {
            memcpy(temp->msg, buffer, buffer_len);
            free(new_node);
        } else {
            temp->next = new_node;
            new_node->next = NULL;
        }
    }


    /* PRINT Mwssage */
    temp = client_info[group_no];
    while(temp) {
        printf("%s : %d : Socket = %d : Msg : %s\n", temp->ip_add, temp->port, temp->sock, temp->msg[0]);
        temp = temp->next;
    }
}

void *read_periodic_client_msg (void *args)
{
    client_detail_t *temp;
    int group_no;
    while(1) {
        for(group_no = 1; group_no <= MAX_CONNECTIONS - 1; group_no++) {
            temp = client_info[group_no];
            if(temp) {
                printf("******** Periodic Message : Group %d ******** \n", group_no);
            } else {
                continue;
            }
            while(temp) {
                printf("%s : %d : Socket = %d : Msg : %s\n", temp->ip_add, temp->port, temp->sock, temp->msg[0]);
                temp = temp->next;
            }
        }

        sleep(10);
    }
}

void update_new_message_from_client(int sock, char *buffer, 
                int buffer_len, char *ip_addr, int port)
{
    int group_no;
    client_detail_t *temp;
    int flag = FALSE;

    for(group_no = 1; group_no <= MAX_CONNECTIONS - 1; group_no++) {
        temp = client_info[group_no];
        while(temp) {
            if((temp->port == port) && (temp->sock == sock) &&
                    (strcmp(temp->ip_add, ip_addr)==0)) {
                flag = TRUE;
                memcpy(temp->msg[0], buffer, buffer_len);
                break;
            }
            temp=temp->next;
        }
    }

    if(flag == FALSE) {
        printf("\n This server doesn't belong to any Group \n");
    }
}
