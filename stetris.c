#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <termios.h>
#include <sys/select.h>
#include <linux/input.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <poll.h>
#include <stdint.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>


// The game state can be used to detect what happens on the playfield
#define GAMEOVER   0
#define ACTIVE     (1 << 0)
#define ROW_CLEAR  (1 << 1)
#define TILE_ADDED (1 << 2)

#define FILEPATH_TO_FB "/dev/fb"
#define FILEPATH_TO_JOYSTICK "/dev/input/event"
#define SIZE_OF_MATRIX (64 * sizeof(uint16_t)) // 8x8 tiles, 16 bits each

int frame_buffer = -1; // Default to -1 to indicate uninitialized
int joystick = -1; // Default to -1 to indicate uninitialized
uint16_t *fb_ptr = NULL; // Frame buffer pointer
struct fb_fix_screeninfo fix_info;
struct input_event joystick_event;

// If you extend this structure, either avoid pointers or adjust
// the game logic allocate/deallocate and reset the memory
typedef struct {
  bool occupied;
    int colour;
} tile;


typedef struct {
  unsigned int x;
  unsigned int y;
} coord;

typedef struct {
  coord const grid;                     // playfield bounds
  unsigned long const uSecTickTime;     // tick rate
  unsigned long const rowsPerLevel;     // speed up after clearing rows
  unsigned long const initNextGameTick; // initial value of nextGameTick

  unsigned int tiles; // number of tiles played
  unsigned int rows;  // number of rows cleared
  unsigned int score; // game score
  unsigned int level; // game level

  tile *rawPlayfield; // pointer to raw memory of the playfield
  tile **playfield;   // This is the play field array
  unsigned int state;
  coord activeTile;                       // current tile

  unsigned long tick;         // incremeted at tickrate, wraps at nextGameTick
                              // when reached 0, next game state calculated
  unsigned long nextGameTick; // sets when tick is wrapping back to zero
                              // lowers with increasing level, never reaches 0
} gameConfig;


gameConfig game = {
                   .grid = {8, 8},
                   .uSecTickTime = 10000,
                   .rowsPerLevel = 2,
                   .initNextGameTick = 50,
};

int colours[] = {
        0xF800,  // Red
        0x07E0,  // Green
        0x001F,  // Blue
        0xFFE0,  // Yellow
        0x8010,  // Purple
        0x07FF,  // Cyan
        0xFDA0,  // Orange
        0xF81F   // Pink
    };

uint16_t random_Colour() {
    return colours[rand() % 8];
}

int getCount(const char* dirPath, const char* prefix) {
    DIR *dir = opendir(dirPath);
    if (dir == NULL) {
        return -1;  // Error opening directory
    }

    int count = 0;
    struct dirent *entry;

    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, prefix, strlen(prefix)) == 0) {
            count++;
        }
    }

    closedir(dir);
    return count;
}

bool getFrameBuffer()
{
    int framebufferDescriptor;  // Renamed from fb
    int numberOfFrameBuffers = getCount("/dev", "fb");
    for (size_t bufferIndex = 0; bufferIndex < numberOfFrameBuffers; bufferIndex++)
    {
        char bufferPath[256] = {};

        // Constructing the path to the framebuffer
        snprintf(bufferPath, sizeof(bufferPath), "%s%zu", FILEPATH_TO_FB, bufferIndex);

        // Opening the framebuffer
        framebufferDescriptor = open(bufferPath, O_RDWR);
        
        if (framebufferDescriptor == -1)  // Check for -1 as a sign of an error
        {
            continue;  // Skip to the next framebuffer
        }

        // Filling in the info of the framebuffer's fixed screen info
        if (ioctl(framebufferDescriptor, FBIOGET_FSCREENINFO, &fix_info) < 0)  // Check if ioctl succeeded
        {
            close(framebufferDescriptor);  // Close the opened device before moving on
            continue;  // Move on to the next framebuffer
        }

        // Check if the framebuffer ID matches the desired one
        if (strcmp(fix_info.id, "RPi-Sense FB") == 0)
        {
            frame_buffer = framebufferDescriptor;  // Storing the framebuffer descriptor
            return true;  // Successfully found the framebuffer
        }
        else
        {
            close(framebufferDescriptor);  // Close the opened framebuffer since it's not the desired one
        }
    }

    return false;  // If the loop completes, the framebuffer was not found
}

bool getJoystick() {
    int joystickDescriptor;  
    int numberOfInputDevices = getCount("/dev/input", "event");
    for (size_t deviceIndex = 0; deviceIndex < numberOfInputDevices; deviceIndex++)
    {
        char devicePath[256] = {};
        char deviceName[256] = {};  

        // Constructing the path to the device
        snprintf(devicePath, sizeof(devicePath), "%s%zu", FILEPATH_TO_JOYSTICK, deviceIndex);  // Using snprintf for safety

        // Opening the device with O_NONBLOCK
        joystickDescriptor = open(devicePath, O_RDWR | O_NONBLOCK);
        
        if (joystickDescriptor == -1)  // Check for -1 as a sign of an error
        {
            continue;  // Skip to the next device
        }

        // Getting the name of the device
        if (ioctl(joystickDescriptor, EVIOCGNAME(sizeof(deviceName)), deviceName) < 0)  // Check if ioctl succeeded
        {
            close(joystickDescriptor);  // Close the opened device before moving on
            continue;  // Move on to the next device
        }

        // Check if the device name matches the Raspberry Pi joystick
        if (strcmp(deviceName, "Raspberry Pi Sense HAT Joystick") == 0)
        {
            joystick = joystickDescriptor;  // Storing the joystick file descriptor
            return true;  // Successfully found the joystick
        }
        else
        {
            close(joystickDescriptor);  // Close the opened device since it's not the desired joystick
        }
    }

    return false;  // If the loop completes, the joystick was not found
}

bool initializeSenseHat() {
    if (!getFrameBuffer()) {
        printf("Error: Could not find framebuffer\n");
        return false;
    }

    fb_ptr = mmap(NULL, SIZE_OF_MATRIX, PROT_READ | PROT_WRITE, MAP_SHARED, frame_buffer, 0);
    if (fb_ptr == MAP_FAILED) {
        close(frame_buffer);
        printf("Error: Failed to map framebuffer to memory\n");
        return false;
    }


 

    // Initialize joystick
    if (!getJoystick()) {
        printf("Could not find joystick\n");
        return false;
    }

    return true;
}


// This function is called when the application exits
// Here you can free up everything that you might have opened/allocated
void freeSenseHat() {
    // Unmap the framebuffer from virtual memory
    if (fb_ptr != MAP_FAILED) {
        munmap(fb_ptr, SIZE_OF_MATRIX);
        fb_ptr = NULL;  // Reset the pointer after unmapping
    }

    // Close the framebuffer if opened
    if (frame_buffer != -1) {  // Assuming frame_buffer is initialized to -1 by default
        close(frame_buffer);
        frame_buffer = -1;  // Reset the framebuffer descriptor
    }

    // Close the joystick if opened
    if (joystick != -1) {  // Assuming joystick is initialized to -1 by default
        close(joystick);
        joystick = -1;  // Reset the joystick descriptor
    }
}

int readSenseHatJoystick() {
    struct pollfd js_Poll;
    js_Poll.fd = joystick;
    js_Poll.events = POLLIN;

    if (poll(&js_Poll, 1, 0) > 0) {
    read(joystick, &joystick_event, sizeof(joystick_event));
      if ((joystick_event.value == 1 || joystick_event.value == 2) && joystick_event.type == EV_KEY) {
        return (int)joystick_event.code;
    }
    }
    return 0; 
  }

void renderSenseHatMatrix(bool const playfieldChanged) {
    if (!playfieldChanged) {
        return;
    }

    for (int x = 0; x < 8; x++) {
        for (int y = 0; y < 8; y++) {
            if (game.playfield[y][x].occupied){
                fb_ptr[x + (8 * y)] = game.playfield[y][x].colour;
            }
            else {
                fb_ptr[x + (8 * y)] = 0;
            }
        }
    }
}


// The game logic uses only the following functions to interact with the playfield.
// if you choose to change the playfield or the tile structure, you might need to
// adjust this game logic <> playfield interface

static inline void newTile(coord const target) {
  game.playfield[target.y][target.x].occupied = true;
  game.playfield[target.y][target.x].colour = random_Colour();
}

static inline void copyTile(coord const to, coord const from) {
  memcpy((void *) &game.playfield[to.y][to.x], (void *) &game.playfield[from.y][from.x], sizeof(tile));
}

static inline void copyRow(unsigned int const to, unsigned int const from) {
  memcpy((void *) &game.playfield[to][0], (void *) &game.playfield[from][0], sizeof(tile) * game.grid.x);

}

static inline void resetTile(coord const target) {
  memset((void *) &game.playfield[target.y][target.x], 0, sizeof(tile));
}

static inline void resetRow(unsigned int const target) {
  memset((void *) &game.playfield[target][0], 0, sizeof(tile) * game.grid.x);
}

static inline bool tileOccupied(coord const target) {
  return game.playfield[target.y][target.x].occupied;
}

static inline bool rowOccupied(unsigned int const target) {
  for (unsigned int x = 0; x < game.grid.x; x++) {
    coord const checkTile = {x, target};
    if (!tileOccupied(checkTile)) {
      return false;
    }
  }
  return true;
}


static inline void resetPlayfield() {
  for (unsigned int y = 0; y < game.grid.y; y++) {
    resetRow(y);
  }
}

// Below here comes the game logic. Keep in mind: You are not allowed to change how the game works!
// that means no changes are necessary below this line! And if you choose to change something
// keep it compatible with what was provided to you!

bool addNewTile() {
  game.activeTile.y = 0;
  game.activeTile.x = (game.grid.x - 1) / 2;
  if (tileOccupied(game.activeTile))
    return false;
  newTile(game.activeTile);
  return true;
}

bool moveRight() {
  coord const newTile = {game.activeTile.x + 1, game.activeTile.y};
  if (game.activeTile.x < (game.grid.x - 1) && !tileOccupied(newTile)) {
    copyTile(newTile, game.activeTile);
    resetTile(game.activeTile);
    game.activeTile = newTile;
    return true;
  }
  return false;
}

bool moveLeft() {
  coord const newTile = {game.activeTile.x - 1, game.activeTile.y};
  if (game.activeTile.x > 0 && !tileOccupied(newTile)) {
    copyTile(newTile, game.activeTile);
    resetTile(game.activeTile);
    game.activeTile = newTile;
    return true;
  }
  return false;
}


bool moveDown() {
  coord const newTile = {game.activeTile.x, game.activeTile.y + 1};
  if (game.activeTile.y < (game.grid.y - 1) && !tileOccupied(newTile)) {
    copyTile(newTile, game.activeTile);
    resetTile(game.activeTile);
    game.activeTile = newTile;
    return true;
  }
  return false;
}


bool clearRow() {
  if (rowOccupied(game.grid.y - 1)) {
    for (unsigned int y = game.grid.y - 1; y > 0; y--) {
      copyRow(y, y - 1);
    }
    resetRow(0);
    return true;
  }
  return false;
}

void advanceLevel() {
  game.level++;
  switch(game.nextGameTick) {
  case 1:
    break;
  case 2 ... 10:
    game.nextGameTick--;
    break;
  case 11 ... 20:
    game.nextGameTick -= 2;
    break;
  default:
    game.nextGameTick -= 10;
  }
}

void newGame() {
  game.state = ACTIVE;
  game.tiles = 0;
  game.rows = 0;
  game.score = 0;
  game.tick = 0;
  game.level = 0;
  resetPlayfield();
}

void gameOver() {
  game.state = GAMEOVER;
  game.nextGameTick = game.initNextGameTick;
}


bool sTetris(int const key) {
  bool playfieldChanged = false;

  if (game.state & ACTIVE) {
    // Move the current tile
    if (key) {
      playfieldChanged = true;
      switch(key) {
      case KEY_LEFT:
        moveLeft();
        break;
      case KEY_RIGHT:
        moveRight();
        break;
      case KEY_DOWN:
        while (moveDown()) {};
        game.tick = 0;
        break;
      default:
        playfieldChanged = false;
      }
    }

    // If we have reached a tick to update the game
    if (game.tick == 0) {
      // We communicate the row clear and tile add over the game state
      // clear these bits if they were set before
      game.state &= ~(ROW_CLEAR | TILE_ADDED);

      playfieldChanged = true;
      // Clear row if possible
      if (clearRow()) {
        game.state |= ROW_CLEAR;
        game.rows++;
        game.score += game.level + 1;
        if ((game.rows % game.rowsPerLevel) == 0) {
          advanceLevel();
        }
      }

      // if there is no current tile or we cannot move it down,
      // add a new one. If not possible, game over.
      if (!tileOccupied(game.activeTile) || !moveDown()) {
        if (addNewTile()) {
          game.state |= TILE_ADDED;
          game.tiles++;
        } else {
          gameOver();
        }
      }
    }
  }

  // Press any key to start a new game
  if ((game.state == GAMEOVER) && key) {
    playfieldChanged = true;
    newGame();
    addNewTile();
    game.state |= TILE_ADDED;
    game.tiles++;
  }

  return playfieldChanged;
}

void renderConsole(bool const playfieldChanged) {
  if (!playfieldChanged)
    return;

  // Goto beginning of console
  fprintf(stdout, "\033[%d;%dH", 0, 0);
  for (unsigned int x = 0; x < game.grid.x + 2; x ++) {
    fprintf(stdout, "-");
  }
  fprintf(stdout, "\n");
  for (unsigned int y = 0; y < game.grid.y; y++) {
    fprintf(stdout, "|");
    for (unsigned int x = 0; x < game.grid.x; x++) {
      coord const checkTile = {x, y};
      fprintf(stdout, "%c", (tileOccupied(checkTile)) ? '#' : ' ');
    }
    switch (y) {
      case 0:
        fprintf(stdout, "| Tiles: %10u\n", game.tiles);
        break;
      case 1:
        fprintf(stdout, "| Rows:  %10u\n", game.rows);
        break;
      case 2:
        fprintf(stdout, "| Score: %10u\n", game.score);
        break;
      case 4:
        fprintf(stdout, "| Level: %10u\n", game.level);
        break;
      case 7:
        fprintf(stdout, "| %17s\n", (game.state == GAMEOVER) ? "Game Over" : "");
        break;
    default:
        fprintf(stdout, "|\n");
    }
  }
  for (unsigned int x = 0; x < game.grid.x + 2; x++) {
    fprintf(stdout, "-");
  }
  fflush(stdout);
}


inline unsigned long uSecFromTimespec(struct timespec const ts) {
  return ((ts.tv_sec * 1000000) + (ts.tv_nsec / 1000));
}

int main(int argc, char **argv) {
  (void) argc;
  (void) argv;
  // This sets the stdin in a special state where each
  // keyboard press is directly flushed to the stdin and additionally
  // not outputted to the stdout
  {
    struct termios ttystate;
    tcgetattr(STDIN_FILENO, &ttystate);
    ttystate.c_lflag &= ~(ICANON | ECHO);
    ttystate.c_cc[VMIN] = 1;
    tcsetattr(STDIN_FILENO, TCSANOW, &ttystate);
  }

  // Allocate the playing field structure
  game.rawPlayfield = (tile *) malloc(game.grid.x * game.grid.y * sizeof(tile));
  game.playfield = (tile**) malloc(game.grid.y * sizeof(tile *));
  if (!game.playfield || !game.rawPlayfield) {
    fprintf(stderr, "ERROR: could not allocate playfield\n");
    return 1;
  }
  for (unsigned int y = 0; y < game.grid.y; y++) {
    game.playfield[y] = &(game.rawPlayfield[y * game.grid.x]);
  }

  // Reset playfield to make it empty
  resetPlayfield();
  // Start with gameOver
  gameOver();

  if (!initializeSenseHat()) {
    fprintf(stderr, "ERROR: could not initilize sense hat\n");
    return 1;
  };

  // Clear console, render first time
  fprintf(stdout, "\033[H\033[J");
  renderConsole(true);
  renderSenseHatMatrix(true);

  while (true) {
    struct timeval sTv, eTv;
    gettimeofday(&sTv, NULL);

    int key = readSenseHatJoystick();
    if (key == KEY_ENTER)
      break;

    bool playfieldChanged = sTetris(key);
    renderConsole(playfieldChanged);
    renderSenseHatMatrix(playfieldChanged);

    // Wait for next tick
    gettimeofday(&eTv, NULL);
    unsigned long const uSecProcessTime = ((eTv.tv_sec * 1000000) + eTv.tv_usec) - ((sTv.tv_sec * 1000000 + sTv.tv_usec));
    if (uSecProcessTime < game.uSecTickTime) {
      usleep(game.uSecTickTime - uSecProcessTime);
    }
    game.tick = (game.tick + 1) % game.nextGameTick;
  }

  freeSenseHat();
  free(game.playfield);
  free(game.rawPlayfield);

  return 0;
}
