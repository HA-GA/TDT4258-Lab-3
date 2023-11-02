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


int fb = 0;
int js = 0;
u_int16_t *fb_ptr = 0;


// The game state can be used to detect what happens on the playfield
#define GAMEOVER   0
#define ACTIVE     (1 << 0)
#define ROW_CLEAR  (1 << 1)
#define TILE_ADDED (1 << 2)

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

int random_Colour() {
    int colours[] = {
        0xFF0000,  // Red
        0x00FF00,  // Green
        0x0000FF,  // Blue
        0xFFFF00,  // Yellow
        0x800080,  // Purple
        0x00FFFF,  // Cyan
        0xFFA500,  // Orange
        0xFFC0CB   // Pink
    };

    int numberOfColours = 8;

    int random = rand() % numberOfColours;

    return colours[random];
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



bool getFrameBuffer()
{
    int framebufferDescriptor;  // Renamed from fb
    int numberOfFrameBuffers = getCount("/dev", "fb");
    for (size_t bufferIndex = 0; bufferIndex < numberOfFrameBuffers; bufferIndex++)
    {
        char bufferPath[256] = {};
        char bufferId[256] = {};  // ID of the framebuffer we're trying to identify

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



// This function is called on the start of your application
// Here you can initialize what ever you need for your task
// return false if something fails, else true
bool initializeSenseHat() {
    if (!setFrameBuffer()) {
        printf("Error: Could not find framebuffer\n");
        return false;
    }

    fb_ptr = mmap(NULL, SIZE_OF_MATRIX, PROT_READ | PROT_WRITE, MAP_SHARED, frame_buffer, 0);
    if (fb_ptr == MAP_FAILED) {
        close(frame_buffer);
        printf("Error: Failed to map framebuffer to memory\n");
        return false;
    }

    // Turn off the LED lights on the Sense HAT
    memset(fb_ptr, 0, SIZE_OF_MATRIX);

    // Initialize joystick
    if (!setJoystick()) {
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


// This function should return the key that corresponds to the joystick press
// KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, with the respective direction
// and KEY_ENTER, when the the joystick is pressed
// !!! when nothing was pressed you MUST return 0 !!!
#include <fcntl.h>
#include <unistd.h>
#include <linux/input.h>

int readSenseHatJoystick()
{
    struct input_event input_event;

    int flags = fcntl(joystick, F_GETFL, 0);
    fcntl(joystick, F_SETFL, flags | O_NONBLOCK);

    ssize_t bytesRead = read(joystick, &input_event, sizeof(input_event));
    if (bytesRead != sizeof(input_event)) {
        return 0; // Return 0 or another code to indicate no valid event was read
    }

    if (input_event.value == 1 || input_event.value == 2) {
        switch (input_event.code) {
            case 106:
                return KEY_RIGHT;
            case 105:
                return KEY_LEFT;
            case 103:  // Assuming 103 is the code for KEY_UP
                return KEY_UP;
            case 108:
                return KEY_DOWN;
            case 28:
                return KEY_ENTER;
            default:
                return 0;  // Unrecognized code
        }
    }

    return 0;  // Default return value if no recognized joystick event
}



// This function should render the gamefield on the LED matrix. It is called
// every game tick. The parameter playfieldChanged signals whether the game logic
// has changed the playfield
#include <string.h>

// Assuming game field size is 8x8 for the Sense Hat
#define FIELD_SIZE 8

// Assuming a representation for the game field
// 0 represents empty, and non-zero values represent different game elements/colors
uint8_t gameField[FIELD_SIZE][FIELD_SIZE];

// Assuming you have a function to get the color for each game element
uint32_t getColorForElement(uint8_t element);

void renderSenseHatMatrix(bool const playfieldChanged) {
    if (!playfieldChanged) {
        return; // If the playfield hasn't changed, no need to re-render
    }

    uint32_t matrixColors[FIELD_SIZE * FIELD_SIZE];

    for (int y = 0; y < FIELD_SIZE; y++) {
        for (int x = 0; x < FIELD_SIZE; x++) {
            matrixColors[y * FIELD_SIZE + x] = getColorForElement(gameField[y][x]);
        }
    }

    // Assuming you have a function to set the colors on the Sense Hat
    setSenseHatColors(matrixColors);
}

uint32_t getColorForElement(uint8_t element) {
    switch (element) {
        case 0: // Empty
            return 0x000000; // Black
        case 1: // Some game element
            return 0xFF0000; // Red
        // ... add other game elements and their colors here
        default:
            return 0x000000; // Default to black for unrecognized elements
    }
}

// This function would be an interface to the actual Sense Hat API to set the pixel colors
void setSenseHatColors(uint32_t colors[]) {
    // Here you would interact with the Sense Hat API to set the LED matrix colors
    // For example, using the Sense Hat C library, you might do something like:
    // senseSetPixels(colors);
}


// The game logic uses only the following functions to interact with the playfield.
// if you choose to change the playfield or the tile structure, you might need to
// adjust this game logic <> playfield interface

static inline void newTile(coord const target) {
  game.playfield[target.y][target.x].occupied = true;
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

int readKeyboard() {
  struct pollfd pollStdin = {
       .fd = STDIN_FILENO,
       .events = POLLIN
  };
  int lkey = 0;

  if (poll(&pollStdin, 1, 0)) {
    lkey = fgetc(stdin);
    if (lkey != 27)
      goto exit;
    lkey = fgetc(stdin);
    if (lkey != 91)
      goto exit;
    lkey = fgetc(stdin);
  }
 exit:
    switch (lkey) {
      case 10: return KEY_ENTER;
      case 65: return KEY_UP;
      case 66: return KEY_DOWN;
      case 67: return KEY_RIGHT;
      case 68: return KEY_LEFT;
    }
  return 0;
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
    if (!key)
      key = readKeyboard();
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
