/* Correction du TP de programmation système UNIX

 TP serveur HTTP multi-thread

 Question 1

 -
 Antoine Miné
 06/05/2007
 */

#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <fcntl.h>
#include <time.h>
#include <pthread.h>
#include <stdarg.h>

#define HTTP_LIVE_MSG 			"hc_msg"
#define HTTP_LIVE_STS 			"hc_sts"
#define HTTP_LIVE_STS_SHORT 	"hc_sts_short"
#define HTTP_LIVE_CMD 			"hc_cmd"
#define HTTP_LIVE_TMP 			"hc_tmp"
#define HTTP_LIVE_AMP 			"hc_amp"
#define HTTP_THERMOSTAT 		"hc_thermostat"
#define HTTP_MQTT_TEMP 		    "hc_mqtt_temp"

extern int http_hold; 
extern pthread_mutex_t mutex ;

/* affiche un message d'erreur, errno et quitte */
void fatal_error(const char* msg)
{
	fprintf(stderr, "%s (errno: %s)\n", msg, strerror(errno));
	exit(1);
}

/* crée une socket d'écoute sur le port indiqué
 retourne son descripteur
 */
int cree_socket_ecoute(int port)
{
	int listen_fd; /* socket */
	struct sockaddr_in addr; /* adresse IPv4 d'écoute */
	int one = 1; /* utilisé avec setsockopt */

	/* crée une socket TCP */
	listen_fd = socket(PF_INET, SOCK_STREAM, 0);
	if (listen_fd == -1)
		fatal_error("echec de socket");

	/* évite le délai entre deux bind successifs sur le même port */
	if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) == -1)
		fatal_error("echec de setsockopt(SO_REUSEADDR)");

	/* remplit l'adresse addr */
	memset(&addr, 0, sizeof(addr)); /* initialisation �  0 */
	addr.sin_family = AF_INET; /* adresse Internet */
	addr.sin_port = htons(port); /* port: 2 octets en ordre de réseau */
	/* on accepte les connections sur toutes les adresses IP de la machine */
	addr.sin_addr.s_addr = htonl(INADDR_ANY);

	/* ancre listen_fd �  l'adresse addr */
	if (bind(listen_fd, (const struct sockaddr*) &addr, sizeof(addr)) == -1)
		fatal_error("echec de bind");

	/* transforme liste_fd en une socket d'écoute */
	if (listen(listen_fd, 15) == -1)
		fatal_error("echec de listen");

	fprintf(stderr, "Serveur actif sur le port %i\n", port);

	return listen_fd;
}

/* termine la connection: ferme la socket et termine la thread */
void fin_connection(FILE* stream, char* msg)
{
	fclose(stream);
	pthread_exit(NULL);
}

/* lit une ligne terminée par \r\n */
void my_fgets(char* buf, int size, FILE* stream)
{
	if (!fgets(buf, size - 1, stream))
		fin_connection(stream, "echec de fgets");
}

/* lit une ligne de la forme
 GET /chemin HTTP/1.1
 et stocke 'chemin' dans le buffer url de taille size
 */
void lit_get(FILE* stream, char* url, int size)
{
	char buf[4096];
	int i, j;

	/* lit la requête */
	my_fgets(buf, sizeof(buf), stream);

	/* extrait l'URL et stocke-la dans url */
	if (strncmp(buf, "GET", 3))
		fin_connection(stream, "la requete n'est pas GET");
	for (i = 3; buf[i] == ' '; i++)
		;
	if (!strncmp(buf + i, "http://", 7))
		i += 7;
	for (; buf[i] && buf[i] != '/'; i++)
		;
	if (buf[i] == '/')
		i++;
	for (j = 0; buf[i] && buf[i] != ' ' && j < size - 1; j++, i++)
		url[j] = buf[i];
	url[j] = 0;
	for (; buf[i] == ' '; i++)
		;
	if (strncmp(buf + i, "HTTP/1.", 7))
		fin_connection(stream, "la version n'est pas HTTP/1.1");
}

/* lit les en-têtes
 renvoie 0 si l'en-tête 'connection: close' a été trouvée, 1 sinon
 */
int lit_en_tetes(FILE* stream)
{
	char buf[4096];
	int keepalive = 1;

	while (1)
	{
		my_fgets(buf, sizeof(buf), stream);

		/* fin des en-têtes */
		if (buf[0] == '\n' || buf[0] == '\r')
			break;

		/* détecte l'en-tête 'Connection: close' */
		if (!strncasecmp(buf, "Connection:", 11) || strstr(buf, "close"))
			keepalive = 0;
	}

	return keepalive;
}

/* envoie une erreur 404 et ferme la connection */
void envoie_404(FILE* stream, char* url)
{
	fprintf(stream, "HTTP/1.1 404 Not found\r\n");
	fprintf(stream, "Connection: close\r\n");
	fprintf(stream, "Content-type: text/html\r\n");
	fprintf(stream, "\r\n");
	fprintf(stream, "<html><head><title>Not Found</title></head>"
			"<body><p>Sorry, the object you requested was not found: "
			"<tt>/%s</tt>.</body></html>\r\n", url);
	fin_connection(stream, "erreur 404");
}

/* devine le type MIME d'un fichier */
char* type_fichier(char* chemin)
{
	int len = strlen(chemin);
	if (!strcasecmp(chemin + len - 5, ".html") || !strcasecmp(chemin + len - 4, ".htm"))
		return "text/html";
	if (!strcasecmp(chemin + len - 4, ".css"))
		return "text/css";
	if (!strcasecmp(chemin + len - 4, ".png"))
		return "image/png";
	if (!strcasecmp(chemin + len - 4, ".gif"))
		return "image/gif";
	if (!strcasecmp(chemin + len - 5, ".jpeg") || !strcasecmp(chemin + len - 4, ".jpg"))
		return "image/jpeg";
	return "text/ascii";
}

/* envoie la contenu du fichier */
void envoie_fichier(FILE* stream, char* chemin, int keepalive)
{
	char modiftime[30];
	char curtime[30];
	struct timeval tv;
	char buf[4096];
	struct stat s;
	int fd;

	/* pour des raisons de sécurité, on évite les chemins contenant .. */
	if (strstr(chemin, ".."))
	{
		envoie_404(stream, chemin);
		return;
	}

	/* ouverture et vérifications */
	fd = open(chemin, O_RDONLY);
	if (fd == -1)
	{
		envoie_404(stream, chemin);
		return;
	}
	if (fstat(fd, &s) == -1 || !S_ISREG(s.st_mode) || !(s.st_mode & S_IROTH))
	{
		close(fd);
		envoie_404(stream, chemin);
		return;
	}

	/* calcul des dates */
	if (gettimeofday(&tv, NULL) || !ctime_r(&s.st_mtime, modiftime) || !ctime_r(&tv.tv_sec, curtime))
	{
		close(fd);
		envoie_404(stream, chemin);
		return;
	}
	modiftime[strlen(modiftime) - 1] = 0; /* supprime le \n final */
	curtime[strlen(curtime) - 1] = 0; /* supprime le \n final */

	/* envoie l'en-tête */
	fprintf(stream, "HTTP/1.1 200 OK\r\n");
	fprintf(stream, "Connection: %s\r\n", keepalive ? "keep-alive" : "close");
	fprintf(stream, "Content-length: %li\r\n", (long) s.st_size);
	fprintf(stream, "Content-type: %s\r\n", type_fichier(chemin));
	fprintf(stream, "Date: %s\r\n", curtime);
	fprintf(stream, "Last-modified: %s\r\n", modiftime);
	fprintf(stream, "\r\n");

	/* envoie le corps */
	while (1)
	{
		int r = read(fd, buf, sizeof(buf)), w;
		if (r == 0)
			break;
		if (r < 0)
		{
			if (errno == EINTR)
				continue;
			close(fd);
			fin_connection(stream, "echec de read");
		}
		for (w = 0; w < r;)
		{
			int a = fwrite(buf + w, 1, r - w, stream);
			if (a <= 0)
			{
				if (errno == EINTR)
					continue;
				close(fd);
				fin_connection(stream, "echec de write");
			}
			w += a;
		}
	}

	close(fd);
}


int get_http_cmd(char* bufhttp, int buflen)
{
	int len = 0;
	int ii;
	char buf[512];
	struct tm * timeinfo;

	sprintf(buf, "<html><head><title>Sample \"Hello, World\" Application</title></head>\n");
	strcpy(&bufhttp[len], buf);
	len += strlen(buf);

	sprintf(buf, "<body bgcolor=white>\n");
	strcpy(&bufhttp[len], buf);
	len += strlen(buf);

	// Radiator
	sprintf(buf, "--- Chicken ---\n\n");
	strcpy(&bufhttp[len], buf);
	len += strlen(buf);
	
	sprintf(buf, "<p>\n");
	strcpy(&bufhttp[len], buf);
	len += strlen(buf);

//		
	sprintf(buf, "<a href=\"hc_cmd?open\">Open</a> \n");
	strcpy(&bufhttp[len], buf);
	len += strlen(buf);


	sprintf(buf, "<p>\n");
	strcpy(&bufhttp[len], buf);
	len += strlen(buf);


//		
	sprintf(buf, "<a href=\"hc_cmd?close\">Close</a> \n");
	strcpy(&bufhttp[len], buf);
	len += strlen(buf);

	sprintf(buf, "<p>\n");
	strcpy(&bufhttp[len], buf);
	len += strlen(buf);
	
	
//	
	sprintf(buf, "<a href=\"hc_cmd?Stop\">Stop</a> \n");
	strcpy(&bufhttp[len], buf);
	len += strlen(buf);
	
	sprintf(buf, "<p>\n");
	strcpy(&bufhttp[len], buf);
	len += strlen(buf);
	
	
	 

/*	for (ii = 0; ii < RD_LAST; ii++)
	{

		sprintf(buf, "<p> %s\n", radiateur[ii].name);
		strcpy(&bufhttp[len], buf);
		len += strlen(buf);

		sprintf(buf, "<a href=\"hc_cmd?RAD_%s=15\">15�C</a> \n", radiateur[ii].name, radiateur[ii].name);
		strcpy(&bufhttp[len], buf);
		len += strlen(buf);

		sprintf(buf, "<a href=\"hc_cmd?RAD_%s=20\">20�C</a> \n", radiateur[ii].name, radiateur[ii].name);
		strcpy(&bufhttp[len], buf);
		len += strlen(buf);

		sprintf(buf, "<a href=\"hc_cmd?RAD_%s=22\">22�C</a> \n", radiateur[ii].name, radiateur[ii].name);
		strcpy(&bufhttp[len], buf);
		len += strlen(buf);
	}
*/

	sprintf(buf, "<p>All radiators\n");
	strcpy(&bufhttp[len], buf);
	len += strlen(buf);

 

	  

	// Radiator program
	sprintf(buf, "<p>[Radiator program]\n");
	strcpy(&bufhttp[len], buf);
	len += strlen(buf);

	sprintf(buf, "<p>All radiators\n");
	strcpy(&bufhttp[len], buf);
	len += strlen(buf);
 
 
 

	sprintf(buf, "</body></html>\n");
	strcpy(&bufhttp[len], buf);
	len += strlen(buf);

	return len;
}


int parse_http_cmd_token(char* cmd)
{
	int ii;
	if (strcmp("open", cmd) == 0)
	{
		printf("open\n");
		pthread_mutex_lock(&mutex);
		system("./openChicken");
		http_hold=60;
		pthread_mutex_unlock(&mutex);

	}

	if (strcmp("close", cmd) == 0)
	{
		printf("close\n");
		pthread_mutex_lock(&mutex);
		system("./closeChicken");
		http_hold=60;
		pthread_mutex_unlock(&mutex);
	}
	if (strcmp("stop", cmd) == 0)
	{
		printf("stop\n");
		pthread_mutex_lock(&mutex);
		system("./stopChicken");
		http_hold=60;
		pthread_mutex_unlock(&mutex);
	}

	
}

int parse_http_cmd(char* cmd)
{
	char* pch;
	pch = strtok(cmd, "?");
	pch = strtok(NULL, "?");
	while (pch != NULL)
	{
		parse_http_cmd_token(pch);
		pch = strtok(NULL, "?");
	}
	return 0;
}

void envoie_live_data(FILE* stream, char* chemin, int keepalive)
{
	char short_name[256];
	char modiftime[30] =
	{ 1 };
	char curtime[30] =
	{ 0 };

	int http_length;
	char* bufhttp;

	strncpy(short_name, chemin, sizeof(short_name) - 1);
	if (!strtok(short_name, "?"))
	{
		strcpy(short_name, chemin);
	}

	printf("short_name %s\n", short_name);

	if (strcmp(short_name, HTTP_LIVE_CMD) == 0)
	{
		parse_http_cmd(chemin);
		bufhttp = (char*) malloc(256 * 1024);
		http_length = get_http_cmd(bufhttp, 256 * 1024);

	}
	else if (strcmp(short_name, HTTP_MQTT_TEMP) == 0)
	{
		bufhttp = (char*) malloc(256 * 1024);

		sprintf(bufhttp, "<html><head><title>MQTT Temp received</title></head><body> MQTT Temp received </body></html> ");
		http_length = strlen(bufhttp);
	}	
	else
	{
		envoie_404(stream, chemin);
		return;
	}

	// modiftime[strlen(modiftime)-1] = 0; /* supprime le \n final */
	// curtime[strlen(curtime)-1] = 0;     /* supprime le \n final */

	/* envoie l'en-tête */
	fprintf(stream, "HTTP/1.1 200 OK\r\n");
	fprintf(stream, "Connection: %s\r\n", keepalive ? "keep-alive" : "close");
	fprintf(stream, "Content-length: %i\r\n", http_length);
	fprintf(stream, "Content-type: %s\r\n", "text/html");
	fprintf(stream, "Date: %s\r\n", curtime);
	fprintf(stream, "Last-modified: %s\r\n", modiftime);
	fprintf(stream, "\r\n");

	/* envoie le corps */
	int w;
	for (w = 0; w < http_length;)
	{
		int a = fwrite(bufhttp + w, 1, http_length - w, stream);
		if (a <= 0)
		{
			if (errno == EINTR)
				continue;
			break;
		}
		w += a;
	}

	free(bufhttp);

}

/* traitement d'une connection
 lancé dans une thread séparée par boucle
 */
void* traite_connection(void* arg)
{
	int socket = (int) arg;
	FILE* stream = fdopen(socket, "r+"); /* transforme socket en FILE* */
	char url[4096];
	int keepalive = 1;

	errno = 0;

	/* bufferisation ligne par ligne */
	setlinebuf(stream);

	/* boucle de traitement des requêtes */
	while (keepalive)
	{

		/* lit la requête */
		lit_get(stream, url, sizeof(url));
		keepalive = lit_en_tetes(stream);

		/* envoie la réponse */
		if (strncmp("hc_", url, 3) == 0)
		{
			envoie_live_data(stream, url, keepalive);
		}
		else
		{
			envoie_fichier(stream, url, keepalive);
		}
	}

	/* fin normale */
	fin_connection(stream, "ok");
	return NULL;
}

/* boucle de réception des connections */
void * http_loop(void * arg)
{
	int listen_fd;
	/* supprime SIGPIPE */
	signal(SIGPIPE, SIG_IGN);
	listen_fd = cree_socket_ecoute(8080);

	while (1)
	{
		int client; /* socket connectée */
		pthread_t id; /* thread qui va gérer la connection */

		/* attend une connection */
		client = accept(listen_fd, NULL, 0);
		if (client == -1)
		{
			if (errno == EINTR || errno == ECONNABORTED)
				continue; /* non fatal */
			fatal_error("Echec de accept");
		}

		/* cree une nouvelle thread pour gérer la connection */
		errno = pthread_create(&id, NULL, traite_connection, (void*) client);
		if (errno)
			fatal_error("Echec de pthread_create");

		/* evite d'avoir �  appeler pthread_join */
		errno = pthread_detach(id);
		if (errno)
			fatal_error("Echec de pthread_detach");
	}
}

int http_q_data(int * current_len, char * bufhttp, char *format, ...)
{
	char buf[1024];
	va_list args;

	va_start(args, format);
	vsprintf(buf, format, args);
	va_end(args);

	strcpy(&bufhttp[*current_len], buf);

	*current_len = *current_len + strlen(buf);

	return 0;
}
