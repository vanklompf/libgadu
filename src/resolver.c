/* $Id$ */

/*
 *  (C) Copyright 2001-2008 Wojtek Kaniewski <wojtekka@irc.pl>
 *                          Robert J. Woźny <speedy@ziew.org>
 *                          Arkadiusz Miśkiewicz <arekm@pld-linux.org>
 *                          Tomasz Chiliński <chilek@chilan.com>
 *                          Adam Wysocki <gophi@ekg.chmurka.net>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License Version
 *  2.1 as published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307,
 *  USA.
 */

/**
 * \file resolver.c
 *
 * \brief Funkcje rozwiązywania nazw
 */

#include <sys/wait.h>
#include <netdb.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "libgadu.h"
#include "resolver.h"

#ifdef GG_CONFIG_HAVE_PTHREAD

#include <pthread.h>

/**
 * \internal Funkcja pomocnicza zwalniająca zasoby po rozwiązywaniu nazwy
 * w wątku.
 *
 * \param data Wskaźnik na wskaźnik bufora zaalokowanego w wątku
 */
static void gg_gethostbyname_cleaner(void *data)
{
	char **buf_ptr = (char**) data;

	if (buf_ptr != NULL) {
		free(*buf_ptr);
		*buf_ptr = NULL;
	}
}

#endif /* GG_CONFIG_HAVE_PTHREAD */

/**
 * \internal Odpowiednik \c gethostbyname zapewniający współbieżność.
 *
 * Jeśli dany system dostarcza \c gethostbyname_r, używa się tej wersji, jeśli
 * nie, to zwykłej \c gethostbyname.
 *
 * \param hostname Nazwa serwera
 * \param addr Wskaźnik na rezultat rozwiązywania nazwy
 * \param pthread Flaga blokowania unicestwiania wątku podczas alokacji pamięci
 *
 * \return 0 jeśli się powiodło, -1 w przypadku błędu
 */
int gg_gethostbyname(const char *hostname, struct in_addr *addr, int pthread)
{
#ifdef GG_CONFIG_HAVE_GETHOSTBYNAME_R
	char *buf = NULL;
	char *new_buf = NULL;
	struct hostent he;
	struct hostent *he_ptr = NULL;
	int h_errnop, ret;
	size_t buf_len = 1024;
	int result = -1;
#ifdef GG_CONFIG_HAVE_PTHREAD
	int old_state;
#endif

#ifdef GG_CONFIG_HAVE_PTHREAD
	pthread_cleanup_push(gg_gethostbyname_cleaner, &buf);

	if (pthread)
		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old_state);
#endif

	buf = malloc(buf_len);

#ifdef GG_CONFIG_HAVE_PTHREAD
	if (pthread)
		pthread_setcancelstate(old_state, NULL);
#endif

	if (buf != NULL) {
		while ((ret = gethostbyname_r(hostname, &he, buf, buf_len, &he_ptr, &h_errnop)) == ERANGE) {
			buf_len *= 2;

#ifdef GG_CONFIG_HAVE_PTHREAD
			if (pthread)
				pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old_state);
#endif

			new_buf = realloc(buf, buf_len);

			if (new_buf != NULL)
				buf = new_buf;

#ifdef GG_CONFIG_HAVE_PTHREAD
			if (pthread)
				pthread_setcancelstate(old_state, NULL);
#endif

			if (new_buf == NULL) {
				ret = ENOMEM;
				break;
			}
		}

		if (ret == 0 && he_ptr != NULL) {
			memcpy(addr, he_ptr->h_addr, sizeof(struct in_addr));
#ifdef GG_CONFIG_HAVE_PTHREAD
			if (pthread)
				pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old_state);
#endif

			free(buf);
			buf = NULL;

#ifdef GG_CONFIG_HAVE_PTHREAD
			if (pthread)
				pthread_setcancelstate(old_state, NULL);
#endif

			result = 0;
		}
	}

#ifdef GG_CONFIG_HAVE_PTHREAD
	pthread_cleanup_pop(1);
#endif

	return result;
#else
	struct hostent *he;

	he = gethostbyname(hostname);

	if (he == NULL)
		return -1;

	memcpy(addr, he->h_addr, sizeof(struct in_addr));

	return 0;
#endif /* GG_CONFIG_HAVE_GETHOSTBYNAME_R */
}

/**
 * \internal Struktura przekazywana do wątku rozwiązującego nazwę.
 */
struct gg_resolver_fork_data {
	int pid;		/*< Identyfikator procesu */
};

/**
 * \internal Rozwiązuje nazwę serwera w osobnym procesie.
 *
 * Połączenia asynchroniczne nie mogą blokować procesu w trakcie rozwiązywania
 * nazwy serwera. W tym celu tworzony jest potok, nowy proces i dopiero w nim
 * przeprowadzane jest rozwiązywanie nazwy. Deskryptor strony do odczytu 
 * zapisuje się w strukturze sieci i czeka na dane w postaci struktury
 * \c in_addr. Jeśli nie znaleziono nazwy, zwracana jest \c INADDR_NONE.
 *
 * \param fd Wskaźnik na zmienną, gdzie zostanie umieszczony deskryptor
 *           potoku
 * \param priv_data Wskaźnik na zmienną, gdzie zostanie umieszczony wskaźnik
 *                  do numeru procesu potomnego rozwiązującego nazwę
 * \param hostname Nazwa serwera do rozwiązania
 *
 * \return 0 jeśli się powiodło, -1 w przypadku błędu
 */
static int gg_resolver_fork_start(int *fd, void **priv_data, const char *hostname)
{
	struct gg_resolver_fork_data *data = NULL;
	struct in_addr addr;
	int pipes[2], new_errno;

	gg_debug(GG_DEBUG_FUNCTION, "** gg_resolver_fork_start(%p, %p, \"%s\");\n", fd, priv_data, hostname);

	if (fd == NULL || priv_data == NULL || hostname == NULL) {
		gg_debug(GG_DEBUG_MISC, "// gg_resolver_fork_start() invalid arguments\n");
		errno = EFAULT;
		return -1;
	}

	data = malloc(sizeof(struct gg_resolver_fork_data));

	if (data == NULL) {
		gg_debug(GG_DEBUG_MISC, "// gg_resolver_fork_start() out of memory for resolver data\n");
		return -1;
	}

	if (pipe(pipes) == -1) {
		gg_debug(GG_DEBUG_MISC, "// gg_resolver_fork_start() unable to create pipes (errno=%d, %s)\n", errno, strerror(errno));
		free(data);
		return -1;
	}

	data->pid = fork();

	if (data->pid == -1) {
		new_errno = errno;
		goto cleanup;
	}

	if (data->pid == 0) {
		close(pipes[0]);

		if ((addr.s_addr = inet_addr(hostname)) == INADDR_NONE) {
			/* W przypadku błędu gg_gethostbyname() zwróci -1
                         * i nie zmieni &addr. Tam jest już INADDR_NONE,
                         * więc nie musimy robić nic więcej. */
			gg_gethostbyname(hostname, &addr, 0);
		}

		if (write(pipes[1], &addr, sizeof(addr)) != sizeof(addr))
			exit(1);

		exit(0);
	}

	close(pipes[1]);

	gg_debug(GG_DEBUG_MISC, "// gg_resolver_fork_start() %p\n", data);

	*fd = pipes[0];
	*priv_data = data;

	return 0;

cleanup:
	if (data)
		free(data);

	close(pipes[0]);
	close(pipes[1]);

	errno = new_errno;

	return -1;
}

/**
 * \internal Usuwanie zasobów po procesie rozwiązywaniu nazwy.
 *
 * Funkcja wywoływana po zakończeniu rozwiązanywania nazwy lub przy zwalnianiu
 * zasobów sesji podczas rozwiązywania nazwy.
 *
 * \param priv_data Wskaźnik na zmienną przechowującą wskaźnik do prywatnych
 *                  danych
 * \param force Flaga usuwania zasobów przed zakończeniem działania
 */
void gg_resolver_fork_cleanup(void **priv_data, int force)
{
	struct gg_resolver_fork_data *data;

	if (priv_data == NULL || *priv_data == NULL)
		return;

	data = (struct gg_resolver_fork_data*) *priv_data;
	*priv_data = NULL;

	if (force)
		kill(data->pid, SIGKILL);

	waitpid(data->pid, NULL, WNOHANG);

	free(data);
}

#ifdef GG_CONFIG_HAVE_PTHREAD

/**
 * \internal Struktura przekazywana do wątku rozwiązującego nazwę.
 */
struct gg_resolver_pthread_data {
	pthread_t thread;	/*< Identyfikator wątku */
	char *hostname;		/*< Nazwa serwera */
	int rfd;		/*< Deskryptor do odczytu */
	int wfd;		/*< Deskryptor do zapisu */
};

/**
 * \internal Usuwanie zasobów po wątku rozwiązywaniu nazwy.
 *
 * Funkcja wywoływana po zakończeniu rozwiązanywania nazwy lub przy zwalnianiu
 * zasobów sesji podczas rozwiązywania nazwy.
 *
 * \param priv_data Wskaźnik na zmienną przechowującą wskaźnik do prywatnych
 *                  danych
 * \param force Flaga usuwania zasobów przed zakończeniem działania
 */
static void gg_resolver_pthread_cleanup(void **priv_data, int force)
{
	struct gg_resolver_pthread_data *data;

	if (priv_data == NULL || *priv_data == NULL)
		return;

	data = (struct gg_resolver_pthread_data *) *priv_data;
	*priv_data = NULL;

	if (force) {
		pthread_cancel(data->thread);
		pthread_join(data->thread, NULL);
	}

	free(data->hostname);
	data->hostname = NULL;

	if (data->wfd != -1) {
		close(data->wfd);
		data->wfd = -1;
	}

	free(data);
}

/**
 * \internal Wątek rozwiązujący nazwę.
 *
 * \param arg Wskaźnik na strukturę \c gg_resolver_pthread_data
 */
static void *gg_resolver_pthread_thread(void *arg)
{
	struct gg_resolver_pthread_data *data = arg;
	struct in_addr addr;

	pthread_detach(pthread_self());

	if ((addr.s_addr = inet_addr(data->hostname)) == INADDR_NONE) {
		/* W przypadku błędu gg_gethostbyname() zwróci -1
                 * i nie zmieni &addr. Tam jest już INADDR_NONE,
                 * więc nie musimy robić nic więcej. */
		gg_gethostbyname(data->hostname, &addr, 1);
	}

	if (write(data->wfd, &addr, sizeof(addr)) == sizeof(addr))
		pthread_exit(NULL);
	else 
		pthread_exit((void*) -1);

	return NULL;	/* żeby kompilator nie marudził */
}

/**
 * \internal Rozwiązuje nazwę serwera w osobnym wątku.
 *
 * Funkcja działa analogicznie do \c gg_resolver_fork_start(), z tą różnicą,
 * że działa na wątkach, nie procesach. Jest dostępna wyłącznie gdy podczas
 * kompilacji włączono odpowiednią opcję.
 *
 * \param fd Wskaźnik na zmienną, gdzie zostanie umieszczony deskryptor
 *           potoku
 * \param priv_data Wskaźnik na zmienną, gdzie zostanie umieszczony wskaźnik
 *                  do prywatnych danych wątku rozwiązującego nazwę
 * \param hostname Nazwa serwera do rozwiązania
 *
 * \return 0 jeśli się powiodło, -1 w przypadku błędu
 */
static int gg_resolver_pthread_start(int *fd, void **priv_data, const char *hostname)
{
	struct gg_resolver_pthread_data *data = NULL;
	int pipes[2], new_errno;

	gg_debug(GG_DEBUG_FUNCTION, "** gg_resolver_pthread_start(%p, %p, \"%s\");\n", fd, priv_data, hostname);

	if (fd == NULL || priv_data == NULL || hostname == NULL) {
		gg_debug(GG_DEBUG_MISC, "// gg_resolver_pthread_start() invalid arguments\n");
		errno = EFAULT;
		return -1;
	}

	data = malloc(sizeof(struct gg_resolver_pthread_data));

	if (data == NULL) {
		gg_debug(GG_DEBUG_MISC, "// gg_resolver_pthread_start() out of memory for resolver data\n");
		return -1;
	}

	if (pipe(pipes) == -1) {
		gg_debug(GG_DEBUG_MISC, "// gg_resolver_pthread_start() unable to create pipes (errno=%d, %s)\n", errno, strerror(errno));
		free(data);
		return -1;
	}

	data->hostname = strdup(hostname);

	if (data->hostname == NULL) {
		gg_debug(GG_DEBUG_MISC, "// gg_resolver_pthread_start() out of memory\n");
		new_errno = errno;
		goto cleanup;
	}

	data->rfd = pipes[0];
	data->wfd = pipes[1];

	if (pthread_create(&data->thread, NULL, gg_resolver_pthread_thread, data)) {
		gg_debug(GG_DEBUG_MISC, "// gg_resolver_pthread_start() unable to create thread\n");
		new_errno = errno;
		goto cleanup;
	}

	gg_debug(GG_DEBUG_MISC, "// gg_resolver_pthread_start() %p\n", data);

	*fd = pipes[0];
	*priv_data = data;

	return 0;

cleanup:
	if (data) {
		free(data->hostname);
		free(data);
	}

	close(pipes[0]);
	close(pipes[1]);

	errno = new_errno;

	return -1;
}

#endif /* GG_CONFIG_HAVE_PTHREAD */

/**
 * Ustawia sposób rozwiązywania nazw w sesji.
 *
 * \param gs Struktura sesji
 * \param type Sposób rozwiązywania nazw (patrz \ref build-resolver)
 *
 * \return 0 jeśli się powiodło, -1 w przypadku błędu
 */
int gg_session_set_resolver(struct gg_session *gs, gg_resolver_t type)
{
	if (gs == NULL) {
		errno = EINVAL;
		return -1;
	}

	if (type == GG_RESOLVER_DEFAULT) {
#if !defined(GG_CONFIG_HAVE_PTHREAD) || !defined(GG_CONFIG_PTHREAD_DEFAULT)
		type = GG_RESOLVER_FORK;
#else
		type = GG_RESOLVER_PTHREAD;
#endif
	}

	switch (type) {
		case GG_RESOLVER_FORK:
			gs->resolver_type = type;
			gs->resolver_start = gg_resolver_fork_start;
			gs->resolver_cleanup = gg_resolver_fork_cleanup;
			return 0;

#ifdef GG_CONFIG_HAVE_PTHREAD
		case GG_RESOLVER_PTHREAD:
			gs->resolver_type = type;
			gs->resolver_start = gg_resolver_pthread_start;
			gs->resolver_cleanup = gg_resolver_pthread_cleanup;
			return 0;
#endif

		default:
			errno = EINVAL;
			return -1;
	}
}

/**
 * Zwraca sposób rozwiązywania nazw w sesji.
 *
 * \param gs Struktura sesji
 *
 * \return Sposób rozwiązywania nazw
 */
gg_resolver_t gg_session_get_resolver(struct gg_session *gs)
{
	if (gs == NULL) {
		errno = EINVAL;
		return GG_RESOLVER_INVALID;
	}

	return gs->resolver_type;
}

/**
 * Ustawia własny rozwiązywania nazw w sesji.
 *
 * \param gs Struktura sesji
 * \param resolver_start Funkcja rozpoczynająca rozwiązywanie nazwy
 * \param resolver_cleanup Funkcja zwalniająca zasoby
 *
 * \return 0 jeśli się powiodło, -1 w przypadku błędu
 */
int gg_session_set_custom_resolver(struct gg_session *gs, int (*resolver_start)(int*, void**, const char*), void (*resolver_cleanup)(void**, int))
{
	if (gs == NULL || resolver_start == NULL || resolver_cleanup == NULL) {
		errno = EINVAL;
		return -1;
	}

	gs->resolver_type = GG_RESOLVER_CUSTOM;
	gs->resolver_start = resolver_start;
	gs->resolver_cleanup = resolver_cleanup;

	return 0;
}

/**
 * Ustawia sposób rozwiązywania nazw połączenia HTTP.
 *
 * \param gh Struktura połączenia
 * \param type Sposób rozwiązywania nazw (patrz \ref build-resolver)
 *
 * \return 0 jeśli się powiodło, -1 w przypadku błędu
 */
int gg_http_set_resolver(struct gg_http *gh, gg_resolver_t type)
{
	if (gh == NULL) {
		errno = EINVAL;
		return -1;
	}

	if (type == GG_RESOLVER_DEFAULT) {
#if !defined(GG_CONFIG_HAVE_PTHREAD) || !defined(GG_CONFIG_PTHREAD_DEFAULT)
		type = GG_RESOLVER_FORK;
#else
		type = GG_RESOLVER_PTHREAD;
#endif
	}

	switch (type) {
		case GG_RESOLVER_FORK:
			gh->resolver_type = type;
			gh->resolver_start = gg_resolver_fork_start;
			gh->resolver_cleanup = gg_resolver_fork_cleanup;
			return 0;

#ifdef GG_CONFIG_HAVE_PTHREAD
		case GG_RESOLVER_PTHREAD:
			gh->resolver_type = type;
			gh->resolver_start = gg_resolver_pthread_start;
			gh->resolver_cleanup = gg_resolver_pthread_cleanup;
			return 0;
#endif

		default:
			errno = EINVAL;
			return -1;
	}
}

/**
 * Zwraca sposób rozwiązywania nazw połączenia HTTP.
 *
 * \param gh Struktura połączenia
 *
 * \return Sposób rozwiązywania nazw
 */
gg_resolver_t gg_http_get_resolver(struct gg_http *gh)
{
	if (gh == NULL) {
		errno = EINVAL;
		return GG_RESOLVER_INVALID;
	}

	return gh->resolver_type;
}

/**
 * Ustawia własny rozwiązywania nazw połączenia HTTP.
 *
 * \param gh Struktura sesji
 * \param resolver_start Funkcja rozpoczynająca rozwiązywanie nazwy
 * \param resolver_cleanup Funkcja zwalniająca zasoby
 *
 * \return 0 jeśli się powiodło, -1 w przypadku błędu
 */
int gg_http_set_custom_resolver(struct gg_http *gh, int (*resolver_start)(int*, void**, const char*), void (*resolver_cleanup)(void**, int))
{
	if (gh == NULL || resolver_start == NULL || resolver_cleanup == NULL) {
		errno = EINVAL;
		return -1;
	}

	gh->resolver_type = GG_RESOLVER_CUSTOM;
	gh->resolver_start = resolver_start;
	gh->resolver_cleanup = resolver_cleanup;

	return 0;
}
