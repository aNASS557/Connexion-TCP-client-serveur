#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include "common.h"


int server_fd;
char nick[NICK_LEN] = "";
char current_channel[INFOS_LEN] = "";
char pending_path[MSG_LEN] = "";   

void die(int ret_value, const char *msg) {
    if (ret_value < 0) {
        perror(msg);
        exit(EXIT_FAILURE);
    }
}

static void read_full(int fd, void *ptr, int size) {
    int read_bytes = 0;
    int ret_value;
    while (read_bytes != size) {
        ret_value = read(fd, (char *)ptr + read_bytes, size - read_bytes);
        if (ret_value < 0 && errno == EINTR) continue;
        die(ret_value, "reading on socket");
        if (ret_value == 0) {
            printf("Disconnected\n");
            exit(EXIT_FAILURE);
        }
        read_bytes += ret_value;
    }
}

static void write_full(int fd, const void *ptr, int size) {
    int written_bytes = 0;
    int ret_value;
    while (written_bytes != size) {
        ret_value = write(fd, (const char *)ptr + written_bytes, size - written_bytes);
        if (ret_value < 0 && errno == EINTR) continue;
        die(ret_value, "writing on socket");
        if (ret_value == 0) {
            printf("Disconnected\n");
            exit(EXIT_FAILURE);
        }
        written_bytes += ret_value;
    }
}

static void send_frame(int fd, enum msg_type type, const char *sender,
                       const char *infos, const void *payload, int len) {
    struct message msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = type;
    msg.pld_len = len;
    strncpy(msg.nick_sender, sender, NICK_LEN - 1);
    strncpy(msg.infos, infos, INFOS_LEN - 1);
    write_full(fd, &msg, sizeof(msg));
    if (len > 0) write_full(fd, payload, len);
}

static void receive_frame(int fd, struct message *msg, char *payload) {
    read_full(fd, msg, sizeof(*msg));
    if (msg->pld_len < 0 || msg->pld_len >= MSG_LEN) {
        printf("Message invalide\n");
        exit(EXIT_FAILURE);
    }
    if (msg->pld_len > 0) read_full(fd, payload, msg->pld_len);
    payload[msg->pld_len] = '\0';
}

static int connect_to(const char *host, const char *port) {
    struct addrinfo hints, *res;
    int fd;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0) {
        printf("Adresse du serveur invalide\n");
        exit(EXIT_FAILURE);
    }
    fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    die(fd, "socket");
    die(connect(fd, res->ai_addr, res->ai_addrlen), "connexion au serveur");
    freeaddrinfo(res);
    return fd;
}

/* "/home/toto/a.txt" -> "a.txt" */
static char *base_name(char *path) {
    char *slash = strrchr(path, '/');
    if (slash == NULL) return path;
    return slash + 1;
}

/* Reçoit le fichier envoyé directement par l'autre client */
static void receive_file(int fd, const char *filename) {
    struct message msg;
    char data[MSG_LEN];
    char path[MSG_LEN + 20];
    int out;

    mkdir(".resa1", 0700);
    mkdir(".resa1/inbox", 0700);
    strcpy(path, ".resa1/inbox/");
    strcat(path, filename);
    out = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    die(out, "open");

    printf("Receiving file...\n");
    while (1) {
        receive_frame(fd, &msg, data);
        if (msg.pld_len == 0) break;   /* fin du fichier */
        write_full(out, data, msg.pld_len);
    }
    close(out);
    send_frame(fd, FILE_ACK, "", filename, NULL, 0);
    printf("%s saved in %s\n", filename, path);
}

/* Quelqu'un veut nous envoyer un fichier */
static void handle_file_request(struct message *request, char *filename) {
    char answer[10];
    int listener, peer;
    struct sockaddr_in addr, local;
    socklen_t len;

    if (filename[0] == '\0' || strchr(filename, '/') != NULL) {
        printf("Nom de fichier invalide.\n");
        send_frame(server_fd, FILE_REJECT, nick, request->nick_sender, NULL, 0);
        return;
    }
    printf("%s wants you to accept the transfer of the file named \"%s\". Do you accept? [Y/N]\n",
           request->nick_sender, filename);
    if (fgets(answer, sizeof(answer), stdin) == NULL) answer[0] = 'N';
    if (answer[0] != 'Y' && answer[0] != 'y') {
        send_frame(server_fd, FILE_REJECT, nick, request->nick_sender, NULL, 0);
        printf("Transfer refused.\n");
        return;
    }

    /* socket d'écoute sur un port libre choisi par le système */
    listener = socket(AF_INET, SOCK_STREAM, 0);
    die(listener, "socket");
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = 0;
    die(bind(listener, (struct sockaddr *)&addr, sizeof(addr)), "bind");
    die(listen(listener, 1), "listen");
    len = sizeof(addr);
    die(getsockname(listener, (struct sockaddr *)&addr, &len), "getsockname");

    /* notre adresse IP, telle que le serveur nous voit */
    len = sizeof(local);
    die(getsockname(server_fd, (struct sockaddr *)&local, &len), "getsockname");
    addr.sin_addr = local.sin_addr;

    /* on envoie directement la structure adresse+port à l'autre client */
    send_frame(server_fd, FILE_ACCEPT, nick, request->nick_sender, &addr, sizeof(addr));

    printf("Waiting for direct connection...\n");
    peer = accept(listener, NULL, NULL);
    die(peer, "accept");
    close(listener);
    receive_file(peer, filename);
    close(peer);
}

/* Envoie pending_path directement à l'autre client */
static void send_file(struct sockaddr_in *peer, const char *recipient) {
    char data[FILE_CHUNK];
    char payload[MSG_LEN];
    struct message ack;
    char *filename = base_name(pending_path);
    int file, fd, n;

    file = open(pending_path, O_RDONLY);
    if (file < 0) {
        perror("open");
        return;
    }
    fd = socket(AF_INET, SOCK_STREAM, 0);
    die(fd, "socket");
    if (connect(fd, (struct sockaddr *)peer, sizeof(*peer)) < 0) {
        perror("connexion directe");
        close(file);
        close(fd);
        return;
    }
    printf("Connecting to %s and sending the file...\n", recipient);
    while ((n = read(file, data, FILE_CHUNK)) > 0)
        send_frame(fd, FILE_SEND, nick, filename, data, n);
    die(n, "read file");
    send_frame(fd, FILE_SEND, nick, filename, "", 0);   /* fin du fichier */
    close(file);

    receive_frame(fd, &ack, payload);
    if (ack.type == FILE_ACK)
        printf("%s has received the file.\n", recipient);
    close(fd);
}

/* Ce que l'utilisateur tape. Renvoie 0 pour quitter. */
static int handle_keyboard(void) {
    char line[MSG_LEN];
    char *newline, *space, *path, *end;

    if (fgets(line, sizeof(line), stdin) == NULL) return 0;
    newline = strchr(line, '\n');
    if (newline != NULL) *newline = '\0';
    if (line[0] == '\0') return 1;

    if (strncmp(line, "/nick ", 6) == 0) {
        if (strlen(line + 6) >= NICK_LEN) {
            printf("Pseudo trop long.\n");
        } else {
            send_frame(server_fd, NICKNAME_NEW, nick, line + 6, NULL, 0);
            strcpy(nick, line + 6);
        }
    } else if (strcmp(line, "/who") == 0) {
        send_frame(server_fd, NICKNAME_LIST, nick, "", NULL, 0);
    } else if (strncmp(line, "/whois ", 7) == 0) {
        send_frame(server_fd, NICKNAME_INFOS, nick, line + 7, NULL, 0);
    } else if (strncmp(line, "/msgall ", 8) == 0) {
        send_frame(server_fd, BROADCAST_SEND, nick, "", line + 8, strlen(line + 8));
    } else if (strncmp(line, "/msg ", 5) == 0) {
        space = strchr(line + 5, ' ');
        if (space == NULL) {
            printf("Usage : /msg <pseudo> <message>\n");
        } else {
            *space = '\0';
            send_frame(server_fd, UNICAST_SEND, nick, line + 5, space + 1, strlen(space + 1));
        }
    } else if (strncmp(line, "/create ", 8) == 0) {
        send_frame(server_fd, MULTICAST_CREATE, nick, line + 8, NULL, 0);
    } else if (strcmp(line, "/channel_list") == 0) {
        send_frame(server_fd, MULTICAST_LIST, nick, "", NULL, 0);
    } else if (strncmp(line, "/join ", 6) == 0) {
        send_frame(server_fd, MULTICAST_JOIN, nick, line + 6, NULL, 0);
    } else if (strncmp(line, "/quit ", 6) == 0) {
        send_frame(server_fd, MULTICAST_QUIT, nick, line + 6, NULL, 0);
        current_channel[0] = '\0';
    } else if (strncmp(line, "/send ", 6) == 0) {
        space = strchr(line + 6, ' ');
        if (space == NULL) {
            printf("Usage : /send <pseudo> \"chemin\"\n");
            return 1;
        }
        *space = '\0';
        path = space + 1;
        if (*path == '"') {
            path++;
            end = strrchr(path, '"');
            if (end != NULL) *end = '\0';
        }
        if (access(path, R_OK) < 0) {
            perror(path);
            return 1;
        }
        strcpy(pending_path, path);
        send_frame(server_fd, FILE_REQUEST, nick, line + 6, base_name(path), strlen(base_name(path)));
        printf("Demande de transfert envoyée à %s.\n", line + 6);
    } else if (strcmp(line, "/quit") == 0) {
        return 0;
    } else if (current_channel[0] != '\0') {
        send_frame(server_fd, MULTICAST_SEND, nick, current_channel, line, strlen(line));
    } else {
        send_frame(server_fd, ECHO_SEND, nick, "", line, strlen(line));
    }
    return 1;
}

/* Ce que le serveur nous envoie */
static void handle_server(void) {
    struct message msg;
    char payload[MSG_LEN];
    struct sockaddr_in peer;

    receive_frame(server_fd, &msg, payload);
    if (msg.type == FILE_REQUEST) {
        handle_file_request(&msg, payload);
    } else if (msg.type == FILE_ACCEPT) {
        printf("%s accepted file transfer.\n", msg.nick_sender);
        if (msg.pld_len == (int)sizeof(peer) && pending_path[0] != '\0') {
            memcpy(&peer, payload, sizeof(peer));
            send_file(&peer, msg.nick_sender);
            pending_path[0] = '\0';
        }
    } else if (msg.type == FILE_REJECT) {
        printf("%s refused the file transfer.\n", msg.nick_sender);
        pending_path[0] = '\0';
    } else if (msg.type == MULTICAST_JOIN) {
        strcpy(current_channel, msg.infos);   /* le serveur confirme qu'on est dans le salon */
    } else if (msg.nick_sender[0] != '\0') {
        printf("[%s] : %s\n", msg.nick_sender, payload);
    } else {
        printf("%s\n", payload);
    }
}

int main(int argc, char **argv) {
    const char *host = SERV_ADDR;
    const char *port = SERV_PORT;
    struct pollfd fds[2];
    int running = 1;
    int ret;

    if (argc > 1) host = argv[1];
    if (argc > 2) port = argv[2];

    signal(SIGPIPE, SIG_IGN);   /* write renvoie une erreur au lieu de tuer le programme */
    server_fd = connect_to(host, port);

    fds[0].fd = STDIN_FILENO;
    fds[0].events = POLLIN;
    fds[1].fd = server_fd;
    fds[1].events = POLLIN;
    printf("Connecté. Commandes : /nick pseudo, /who, /whois pseudo, /msg pseudo message, /msgall message, /send pseudo \"chemin\"\n");

    while (running) {
        ret = poll(fds, 2, -1);
        if (ret < 0 && errno == EINTR) continue;
        die(ret, "poll");
        if (fds[0].revents & POLLIN)
            running = handle_keyboard();
        if (running && (fds[1].revents & (POLLIN | POLLHUP | POLLERR)))
            handle_server();
    }
    close(server_fd);
    return 0;
}
