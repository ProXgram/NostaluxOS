#include "snake.h"
#include "terminal.h"
#include "keyboard.h"
#include "timer.h"
#include "sound.h"
#include "graphics.h" 
#include <stdint.h>
#include <stdbool.h>

#define BLOCK_SIZE 16 
#define GRID_MAX_W 80
#define GRID_MAX_H 60
#define MAX_SNAKE (GRID_MAX_W * GRID_MAX_H)

// Logical grid dimensions (calculated at runtime)
static int grid_w;
static int grid_h;

typedef struct {
    int x;
    int y;
} Point;

static Point snake[MAX_SNAKE];
static int snake_len;
static Point fruit;
static int score;
static int dir_x; // -1, 0, 1
static int dir_y; // -1, 0, 1

// Pseudo-random generator
static unsigned long next = 1;
static int rand(void) {
    next = next * 1103515245 + 12345;
    return (unsigned int)(next / 65536) % 32768;
}

static bool spawn_fruit(void) {
    int inner_w = grid_w - 2;
    int inner_h = grid_h - 2;
    int cell_count = inner_w * inner_h;

    if (inner_w <= 0 || inner_h <= 0 || snake_len >= cell_count) {
        return false;
    }

    /*
     * Start at a pseudo-random cell, then scan each playable cell once.
     * This guarantees termination even when only one free cell remains.
     */
    int start = rand() % cell_count;
    for (int offset = 0; offset < cell_count; offset++) {
        int index = (start + offset) % cell_count;
        Point candidate = {
            .x = (index % inner_w) + 1,
            .y = (index / inner_w) + 1,
        };
        bool collision = false;

        for (int i = 0; i < snake_len; i++) {
            if (snake[i].x == candidate.x && snake[i].y == candidate.y) {
                collision = true;
                break;
            }
        }

        if (!collision) {
            fruit = candidate;
            return true;
        }
    }

    return false;
}

static void draw_block(int x, int y, uint32_t color) {
    graphics_fill_rect(x * BLOCK_SIZE, y * BLOCK_SIZE, BLOCK_SIZE, BLOCK_SIZE, color);
}

void snake_game_run(void) {
    // Initialize grid based on actual screen size
    grid_w = graphics_get_width() / BLOCK_SIZE;
    grid_h = graphics_get_height() / BLOCK_SIZE;
    if (grid_w > GRID_MAX_W) grid_w = GRID_MAX_W;
    if (grid_h > GRID_MAX_H) grid_h = GRID_MAX_H;

    terminal_clear();

    if (grid_w < 6 || grid_h < 3) {
        terminal_writestring("Screen is too small for Snake.\n");
        return;
    }
    
    // Init game state
    snake_len = 3;
    snake[0].x = grid_w / 2;     snake[0].y = grid_h / 2;
    snake[1].x = grid_w / 2 - 1; snake[1].y = grid_h / 2;
    snake[2].x = grid_w / 2 - 2; snake[2].y = grid_h / 2;
    dir_x = 1; dir_y = 0;
    score = 0;
    next = (unsigned long)timer_get_ticks(); // Seed
    bool won = !spawn_fruit();

    // Colors (ARGB)
    uint32_t col_wall = 0xFF555555;
    uint32_t col_bg   = 0xFF000000;
    uint32_t col_head = 0xFF00FF00;
    uint32_t col_body = 0xFF00AA00;
    uint32_t col_fruit= 0xFFFF5555;

    // Clear Screen with black
    graphics_fill_rect(0, 0, graphics_get_width(), graphics_get_height(), col_bg);

    // Draw Border
    for (int x = 0; x < grid_w; x++) {
        draw_block(x, 0, col_wall);
        draw_block(x, grid_h-1, col_wall);
    }
    for (int y = 0; y < grid_h; y++) {
        draw_block(0, y, col_wall);
        draw_block(grid_w-1, y, col_wall);
    }

    // Intro Sound
    sound_beep(440, 10);
    sound_beep(554, 10);
    sound_beep(659, 20);

    bool running = !won;
    while (running) {
        // 1. Input Handling (Non-blocking)
        char key = keyboard_poll_char();
        if (key == 'q') break;
        if (key == 'w' && dir_y == 0) { dir_x = 0; dir_y = -1; }
        if (key == 's' && dir_y == 0) { dir_x = 0; dir_y = 1; }
        if (key == 'a' && dir_x == 0) { dir_x = -1; dir_y = 0; }
        if (key == 'd' && dir_x == 0) { dir_x = 1; dir_y = 0; }

        // 2. Game Logic
        Point next_head = { snake[0].x + dir_x, snake[0].y + dir_y };
        bool growing = (next_head.x == fruit.x && next_head.y == fruit.y);

        // Wall Collision
        if (next_head.x <= 0 || next_head.x >= grid_w - 1 ||
            next_head.y <= 0 || next_head.y >= grid_h - 1) {
            running = false;
            sound_beep(100, 50); // Crash sound
            continue;
        }

        // Self Collision
        /*
         * The current tail moves away on a normal step, so it is safe for the
         * head to enter that cell. When growing, the tail remains in place.
         */
        int collision_length = growing ? snake_len : snake_len - 1;
        for (int i = 0; i < collision_length; i++) {
            if (snake[i].x == next_head.x && snake[i].y == next_head.y) {
                running = false;
                sound_beep(100, 50);
                break;
            }
        }
        if (!running) continue;

        // Move Snake
        Point old_tail = snake[snake_len - 1];
        if (!growing) {
            draw_block(old_tail.x, old_tail.y, col_bg);
        }

        for (int i = snake_len - 1; i > 0; i--) {
            snake[i] = snake[i-1];
        }
        snake[0] = next_head;

        // Check Fruit
        if (growing) {
            if (snake_len >= MAX_SNAKE) {
                won = true;
                running = false;
                continue;
            }

            snake[snake_len] = old_tail;
            snake_len++;
            score += 10;
            sound_beep(1000 + (score * 5), 5); // Pickup sound
            if (!spawn_fruit()) {
                won = true;
                running = false;
            }
        }

        // Draw
        draw_block(snake[0].x, snake[0].y, col_head);
        draw_block(snake[1].x, snake[1].y, col_body);
        if (running) draw_block(fruit.x, fruit.y, col_fruit);

        // 3. Delay (Game Speed)
        timer_wait(5);
    }

    // Game Over Screen - Return to terminal
    terminal_set_theme(0x0F, 0x01);
    terminal_clear();
    terminal_writestring(won ? "\n\n   YOU WIN!\n" : "\n\n   GAME OVER\n");
    terminal_writestring("   Score: ");
    terminal_write_uint(score);
    terminal_writestring("\n   Press any key to exit...");
    
    // Flush input
    while(keyboard_poll_char()); 
    // Wait for key
    while(!keyboard_poll_char());
    
    terminal_clear();
}
