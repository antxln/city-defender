/**
 * file: threads.c
 *
 * Utilize C curses library and C POSIX library to create a war game between
 * an attacker (computer-controlled) and defender (user-controlled). User
 * controls shield to block missiles raining down on city.
 *
 */

#define _DEFAULT_SOURCE
#define MAX_DELAY_MS 300

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ncurses.h>
#include <pthread.h>
#include <limits.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>

struct Game *game;
int height, width;
int row = 2, col;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

/**
 * Represents entire game. Contains data of defender, attacker, and city.
 */
struct Game {
    struct Defender *defender;
    struct Attacker *attacker;
    int *layout;
    int size;
    int cap;
    int tallest;
    _Bool gameOver;
};

/**
 * Represents defender
 */
struct Defender {
    _Bool *gameOver;
    char *name;
    const char *shield;
    int shieldY;
    int shieldX;
};

/**
 * Represents attacker
 */
struct Attacker {
    _Bool *gameOver;
    char *name;
    int totalMissiles;
};

/**
 * Represents a missile
 */
struct Missile {
    int x;
    int y;
    char c;
};

/**
 * Free mem alloc'd for defender
 *
 * @param defender pointer to Defender to free
 */
void destroyDefender(struct Defender *defender) {
    if (defender) {
        if (defender->name) free(defender->name);
        free(defender);
    }
}

/**
 * Free mem alloc'd for attacker
 *
 * @param attacker pointer to Attacker to free
 */
void destroyAttacker(struct Attacker *attacker) {
    if (attacker) {
        if (attacker->name) free(attacker->name);
        free(attacker);
    }
}

/**
 * Free mem alloc'd for game
 *
 * @param game pointer to Game to free
 */
void destroyGame(struct Game *game) {
    if (game) {
        if (game->layout) free(game->layout);
	if (game->defender) destroyDefender(game->defender);
        if (game->attacker) destroyAttacker(game->attacker);
        free(game);
    }
}

/**
 * Convert string to integer
 *
 * @param str string to convert
 * @param val pointer to int storing result of function
 * @return 0 unsuccessful conversion
 *         1 successful conversion
 */
int strInt(char *str, int *val) {
    int sum = 0;
    for (size_t i = 0; i < strlen(str); i++) {
	if (i == 0 && str[i] == '-') continue;

	int digit = str[i] - '0';
	if (digit < 0 || digit > 9) return 0;
	if (sum != 0 && (INT_MAX - digit) / sum < 10) return 0;
	sum = sum * 10 + digit;
    }
    if (str[0] == '\0' || (str[0] == '-' && str[1] == '\0')) return 0;
    *val = (str[0] == '-') ? -sum: sum;
    return 1;
}

/**
 * Process config file and store data in Game struct
 *
 * @param filename the config file
 * @return created Game struct
 */
struct Game *createGame(char *filename) {
    FILE *fp = fopen(filename, "r");
    if (fp == NULL) {
	perror(filename);
	exit(EXIT_FAILURE);
    }

    struct Game *game = malloc(sizeof *game);
    if (game == NULL) {
	perror("createGame");
	exit(EXIT_FAILURE);
    }
    struct Defender *defender = malloc(sizeof *defender);
    if (defender == NULL) {
	perror("createGame");
	exit(EXIT_FAILURE);
    }
    struct Attacker *attacker = malloc(sizeof *attacker);
    if (attacker == NULL) {
        perror("createGame");
        exit(EXIT_FAILURE);
    }
    
    game->gameOver = 0;
    game->defender = defender;
    game->attacker = attacker;
    game->cap = 10;
    game->size = 0;
    game->layout = malloc(sizeof *game->layout * game->cap);
    if (game->layout == NULL) {
        perror("createGame");
        exit(EXIT_FAILURE);
    }
    game->tallest = 0;
    
    defender->gameOver = &game->gameOver;
    defender->name = NULL;
    defender->shield = "#####";

    attacker->gameOver = &game->gameOver;
    attacker->name = NULL;

    int line = 0;
    char *str = NULL;
    size_t len = 0;
    ssize_t char_read;
    while ((char_read = getline(&str, &len, fp)) > 0) {
	if (str[0] == '#') continue;
	str[strlen(str) - 1] = '\0'; // strip '\n'
        if (line == 0) defender->name = strndup(str, 80);
	else if (line == 1) attacker->name = strndup(str, 80);
	else if (line == 2) {
	    int retval = strInt(str, &attacker->totalMissiles);
	    if (retval == 0 || attacker->totalMissiles < 0) {
	        if (retval == 0) fprintf(stderr,
				"Error: missing missile specification.\n");
	        else fprintf(stderr, "Error: missile specification < 0.\n");
		free(str);
		fclose(fp);
		destroyGame(game);
		exit(EXIT_FAILURE);
	    }
	    if (attacker->totalMissiles == 0) attacker->totalMissiles = -1;
	} else {
	    char *token = strtok(str, " ");
	    while (token != NULL) {
		if (game->size >= game->cap - 1) {
		    game->cap *= 2;
		    game->layout = realloc(game->layout,
				    sizeof *game->layout * game->cap);
		    if (game->layout == NULL) {
		        perror("createGame");
		        exit(EXIT_FAILURE);
	            }
		}

		if (strInt(token, game->layout + game->size)) {
		    if (game->layout[game->size] > game->tallest) {
			game->tallest = game->layout[game->size];
		    }
		    game->size++;
		}
		token = strtok(NULL, " ");
	    }
	}
	line++;
    }
    if (str != NULL) free(str);
    fclose(fp);
    if (char_read == -1 && (errno == EINVAL || errno == ENOMEM)) {
	perror("createGame");
	exit(EXIT_FAILURE);
    }
    if (line < 4) {
	if (line == 0) fprintf(stderr, "Error: missing defender name.\n");
	else if (line == 1) fprintf(stderr, "Error: missing attacker name.\n");
	else if (line == 2) fprintf(stderr,
			"Error: missing missile specification.\n");
	else fprintf(stderr, "Error: missing city layout.\n");
	destroyGame(game);
	exit(EXIT_FAILURE);
    }
    return game;
}

/**
 * Initialize display for city and defender (shield)
 *
 * @param game container includes information about city layout
 */
void initDisplay(struct Game *game) {
    int prev = 2;
    for (int i = 0; i < width; i++) {
	int curr = (i > game->size - 1) ? 2: game->layout[i];
	if (curr > 2) {
	    if (curr == prev) {
		mvinsch(height - curr, i, '_');
		refresh();
	    } else if (curr < prev) {
		if (mvinch(height - prev, i - 1) == '_') {
		    mvdelch(height - prev, i - 1);
		    refresh();
		    for (int j = height - prev + 1; j <= height - 2; j++) {
		        mvinsch(j, i - 1, '|');
		        refresh();
		    }
		}
		mvinsch(height - curr, i, '_');
		refresh();
	    } else if (curr > prev) {
		for (int j = height - curr + 1; j <= height - 2; j++) {
                    mvinsch(j, i, '|');
                    refresh();
                }
	    }
	} else if (curr == 1 || curr == 2) {
	    if (prev > 2 && mvinch(height - prev, i - 1) == '_') {
		mvdelch(height - prev, i - 1);
		refresh();
		for (int j = height - prev + 1; j <= height - 2; j++) {
		    mvinsch(j, i - 1, '|');
		    refresh();
		}
	    }
	    mvinsch(height - curr, i, '_');
	    refresh();
	}
	prev = curr;
    }
    game->defender->shieldY = height -
	    ((game->tallest < 2) ? 2: game->tallest) - 2;
    game->defender->shieldX = width / 2 - 2; // width >= 4
    for (size_t i = 0; i < strlen(game->defender->shield); i++) {
        mvinsch(game->defender->shieldY, game->defender->shieldX, '#');
	refresh();
    }
}

/**
 * Display message on curses window
 *
 * @param str message to display
 * @param r row of curses window to start message
 * @param c col of curses window to start message
 * @param b whether or not to skip a line
 * @return 0 if unable to write message
 *         1 if successful
 */
int displayMessage(char *str, int *r, int *c, _Bool b) {
    int oldr = *r;
    int oldc = *c;
    if (*r >= height || *r < 0) return 0;
    while (*c == 0 &&
		    mvinch(*r, *c) != ' ' && mvinch(*r, *c) != '|') {
	if (*r == 0 || *r == 1) break;
	if (++(*r) >= height) {
	    *r = oldr;
	    return 0;
	}
    }
    if (*r != 0 && *r != 1 && b && *r > 0 && mvinch(*r - 1, 0) != ' ') {
	if (++(*r) >= height) {
	    *r = oldr;
	    return 0;
	}
    }
    for (size_t i = 0; i < strlen(str); i++) {
	if (*c / width + *r >= height) {
	    *r = oldr;
	    *c = oldc;
	    return 0;
	}
	mvdelch(*c / width + *r, *c % width);
	mvinsch(*c / width + *r, *c % width, str[i]);
	*c += 1;
	refresh();
    }
    *r += *c / width;
    return 1;
}

/**
 * Function for defense thread, controls defender.
 *
 * @param defender defender to control
 * @return NULL
 */
void *startDef(void *defender) {
    struct Defender *ndefender = defender;
    _Bool b = 1;
    flushinp();
    while (!*ndefender->gameOver || b) {
	int c = getch();
	if (c == 'q') {
	    pthread_mutex_lock(&mutex);
	    *ndefender->gameOver = 1;
	    b = 0;
	    pthread_mutex_unlock(&mutex);
	} else if (c == KEY_LEFT) {
	    pthread_mutex_lock(&mutex);
	    if (ndefender->shieldX > 0) {
		mvdelch(ndefender->shieldY, ndefender->shieldX + 4);
		mvinsch(ndefender->shieldY, ndefender->shieldX + 4, ' ');
                refresh();
		ndefender->shieldX--;
		for (int i = 0; i < 5; i++) {
		    mvdelch(ndefender->shieldY, ndefender->shieldX + i);
		    mvinsch(ndefender->shieldY, ndefender->shieldX + i, '#');
		    refresh();
		}
	    }
	    pthread_mutex_unlock(&mutex);
	} else if (c == KEY_RIGHT) {
	    pthread_mutex_lock(&mutex);
	    if (ndefender->shieldX < width - 5) {
		mvdelch(ndefender->shieldY, ndefender->shieldX);
		mvinsch(ndefender->shieldY, ndefender->shieldX, ' ');
                refresh();
		ndefender->shieldX++;
		for (int i = 0; i < 5; i++) {
		    mvdelch(ndefender->shieldY, ndefender->shieldX + i);
		    mvinsch(ndefender->shieldY, ndefender->shieldX + i, '#');
		    refresh();
		}
	    }
	    pthread_mutex_unlock(&mutex);
	}
    }
    pthread_mutex_lock(&mutex);
    if (!displayMessage("The ", &row, &col, 1)) {
	int row_temp = 1;
	displayMessage("The ", &row_temp, &col, 0);
	if (row_temp > row) row = row_temp;
    }
    if (!displayMessage(ndefender->name, &row, &col, 0)) {
	int row_temp = 1;
	displayMessage(ndefender->name, &row_temp, &col, 0);
	if (row_temp > row) row = row_temp;
    }
    if (!displayMessage(" defense has ended.", &row, &col, 0)) {
	int row_temp = 1;
	displayMessage(" defense has ended.", &row_temp, &col, 0);
	if (row_temp > row) row = row_temp;
    }
    col = 0;
    pthread_mutex_unlock(&mutex);
    return NULL;
}

/**
 * Function for a missile thread, controls a missile.
 *
 * @param missile missile to control
 * @return NULL
 */
void *launchMissile(void *missile) {
    struct Missile *nmissile = missile;
    pthread_mutex_lock(&mutex);
    char prevc = (mvinch(nmissile->y, nmissile->x) == '|') ?
	    ' ': mvinch(nmissile->y, nmissile->x);
    mvdelch(nmissile->y, nmissile->x);
    mvinsch(nmissile->y, nmissile->x, nmissile->c);
    pthread_mutex_unlock(&mutex);
    while (nmissile->c != '*') {
        usleep(rand() % MAX_DELAY_MS * 1000);
        pthread_mutex_lock(&mutex);
        if (nmissile->y < height) mvdelch(nmissile->y, nmissile->x);
        if (nmissile->y < height) mvinsch(nmissile->y, nmissile->x, prevc);
	refresh();
	nmissile->y++;
	char c = (nmissile->y >= height) ?
		' ': mvinch(nmissile->y, nmissile->x);
        if (c == '#' ||
		(c == '*' && nmissile->y < height - game->tallest)) {
	    nmissile->c = '*';
	} else if (nmissile->y == height - ((nmissile->x > game->size - 1) ?
				2: game->layout[nmissile->x]) + 1) {
            nmissile->c = '*';
	    if (nmissile->x < game->size &&
			    game->layout[nmissile->x] != 2) {
		game->layout[nmissile->x]--;
	    }
        } else if (c == '|' ||
		c == '_' ||
		c == '?' ||
	        c == '*') {
            prevc = ' ';
        } else {
            prevc = c;
        }
        if (nmissile->y < height) {
	    mvdelch(nmissile->y, nmissile->x);
	}
        if (nmissile->y < height) {
	    mvinsch(nmissile->y, nmissile->x, nmissile->c);
	}
	if (nmissile->c == '*' && nmissile->y <= height) {
	    mvdelch(nmissile->y - 1, nmissile->x);
	    mvinsch(nmissile->y - 1, nmissile->x, '?');
	}
        refresh();
        pthread_mutex_unlock(&mutex);
	if (nmissile->y > height) break;
    }
    return NULL;
}

/**
 * Function for attack thread, controls attacker.
 *
 * @param attacker attacker to control
 * @return NULL
 */
void *startAtk(void *attacker) {
    struct Attacker *nattacker = attacker;
    int packetSize = (width > 32) ? 8: ((width / 4 == 0) ? width: width / 4);
    struct Missile *missiles = calloc(packetSize, sizeof *missiles);
    if (missiles == NULL) {
	int row_tmp = 1;
	pthread_mutex_lock(&mutex);
        displayMessage("startAtk: ", &row_tmp, &col, 0);
	displayMessage(strerror(errno), &row_tmp, &col, 0);
	if (row_tmp > row) row = row_tmp;
	col = 0;
	*nattacker->gameOver = 1;
	pthread_mutex_unlock(&mutex);
    }
    pthread_t tids[packetSize];
    while (!*nattacker->gameOver) {
	int i = 0;
	while (i < packetSize) {
	    usleep(rand() % (MAX_DELAY_MS * 3) * 1000);
	    missiles[i].y = 2;
	    missiles[i].x = rand() % width;
	    missiles[i].c = '|';

	    pthread_create(tids + i, NULL, launchMissile, missiles + i);
	    i++;
	    if (*nattacker->gameOver ||
		    ((nattacker->totalMissiles > 0) ?
		     (--nattacker->totalMissiles == 0): 0)) {
		pthread_mutex_lock(&mutex);
		*nattacker->gameOver = 1;
		pthread_mutex_unlock(&mutex);
		break;
	    }
	}
	for (int n = 0; n < i; n++) {
	    pthread_join(tids[n], NULL);
	}
    }
    pthread_mutex_lock(&mutex);
    if (!displayMessage("The ", &row, &col, 1)) {
        int row_temp = 1;
        displayMessage("The ", &row_temp, &col, 0);
        if (row_temp > row) row = row_temp;
    }
    if (!displayMessage(nattacker->name, &row, &col, 0)) {
        int row_temp = 1;
        displayMessage(nattacker->name, &row_temp, &col, 0);
        if (row_temp > row) row = row_temp;
    }
    if (!displayMessage(" attack has ended.", &row, &col, 0)) {
        int row_temp = 1;
        displayMessage(" attack has ended.", &row_temp, &col, 0);
        if (row_temp > row) row = row_temp;
    }
    col = 0;
    pthread_mutex_unlock(&mutex);
    free(missiles);
    return NULL;
}

/**
 * Entry function for program. Creates game, runs game,
 * and handle game termination
 *
 * @param argc number of commandline arguments
 * @param argv commandline arguments
 * @return 0 on successful execution
 */
int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: threads config-file\n");
        exit(EXIT_FAILURE);
    }
    game = createGame(argv[1]);

    initscr();
    cbreak();
    noecho();
    keypad(stdscr, 1);
    getmaxyx(stdscr, height, width);

    if (height - ((game->tallest < 2) ? 2: game->tallest) - 2 - 1 - 2 < 0) {
	endwin();
	fprintf(stderr,
		"Error: runtime terminal height (%d) shorter than layout.\n",
		height);
	destroyGame(game);
	exit(EXIT_FAILURE);
    }

    srand(time(NULL));
    initDisplay(game);
    pthread_mutex_lock(&mutex);
    int row_temp = 0;
    displayMessage("Enter 'q' to quit at end of attack, or control-C",
		    &row_temp, &col, 0);
    col = 0;
    pthread_mutex_unlock(&mutex);

    pthread_t defTID, atkTID;
    pthread_create(&defTID, NULL, startDef, game->defender);
    pthread_create(&atkTID, NULL, startAtk, game->attacker);
    pthread_join(defTID, NULL);
    pthread_join(atkTID, NULL);

    if (!displayMessage("hit enter to close...", &row, &col, 0)) {
	row_temp = 1;
	displayMessage("hit enter to close...", &row_temp, &col, 0);
	if (row_temp > row) row = row_temp;
    }
    col = 0;
    flushinp();
    while (getch() != '\n');
    endwin();

    destroyGame(game);
    return 0;
}
