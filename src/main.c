#include "../libretro/libretro.h"
#ifdef HAVE_LIBCURL
#include <curl/curl.h>
#endif
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <boolean.h>
#include <time.h>
#include "lodepng.h"
#include "auth.h"
#include "client.h"
#include "config.h"
#include "cube.h"
#include "db.h"
#include "item.h"
#include "map.h"
#include "matrix.h"
#include <noise.h>
#include "sign.h"
#include "util.h"
#include <tinycthread.h>
#include "world.h"
#include "renderer.h"

#ifdef __LIBRETRO__
#include <retro_miscellaneous.h>
#endif

#include "../textures/font_texture.h"
#include "../textures/sign_texture.h"
#include "../textures/sky_texture.h"
#include "../textures/tiles_texture.h"

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

extern retro_input_state_t input_state_cb;
extern retro_environment_t environ_cb;
extern unsigned game_width, game_height;
double glfwGetTime(void);
void glfwSetTime(double val);

unsigned RENDER_CHUNK_RADIUS = 10;
unsigned SHOW_INFO_TEXT = 1;
unsigned JUMPING_FLASH_MODE = 0;
unsigned FIELD_OF_VIEW = 90;
unsigned INVERTED_AIM = 1;
float ANALOG_SENSITIVITY = 0.0200;
float DEADZONE_RADIUS = 0.040;

#define MAX_CHUNKS 8192
#define MAX_PLAYERS 128
#define WORKERS 4
#define MAX_TEXT_LENGTH 256
#define MAX_PATH_LENGTH 256
#define MAX_ADDR_LENGTH 256

#define ALIGN_LEFT 0
#define ALIGN_CENTER 1
#define ALIGN_RIGHT 2

#define MODE_OFFLINE 0
#define MODE_ONLINE 1

#define WORKER_IDLE 0
#define WORKER_BUSY 1
#define WORKER_DONE 2

typedef struct {
    Map map;
    Map lights;
    SignList signs;
    int p;
    int q;
    int faces;
    int sign_faces;
    int dirty;
    int miny;
    int maxy;
    uintptr_t buffer;
    uintptr_t sign_buffer;
} Chunk;

typedef struct {
    int p;
    int q;
    int load;
    Map *block_maps[3][3];
    Map *light_maps[3][3];
    int miny;
    int maxy;
    int faces;
    float *data;
} WorkerItem;

typedef struct {
    int index;
    int state;
    thrd_t thrd;
    mtx_t mtx;
    cnd_t cnd;
    WorkerItem item;
} Worker;

typedef struct {
    int x;
    int y;
    int z;
    int w;
} Block;

typedef struct {
    Worker workers[WORKERS];
    Chunk chunks[MAX_CHUNKS];
    int chunk_count;
    int create_radius;
    int delete_radius;
    int sign_radius;
    Player players[MAX_PLAYERS];
    int player_count;
    int typing;
    char typing_buffer[MAX_TEXT_LENGTH];
    int message_index;
    char messages[MAX_MESSAGES][MAX_TEXT_LENGTH];
    int width;
    int height;
    int observe1;
    int observe2;
    int flying;
    int item_index;
    int scale;
    int ortho;
    float fov;
    int suppress_char;
    int mode;
    int mode_changed;
    char db_path[MAX_PATH_LENGTH];
    char server_addr[MAX_ADDR_LENGTH];
    int server_port;
    int day_length;
    int time_changed;
    Block block0;
    Block block1;
    Block copy0;
    Block copy1;
} Model;

#if 0
#define TEST_FPS
#endif

#ifdef _MSC_VER
#define isnan(x) _isnan(x)
#define signbit(x) (_copysign(1.0, x) < 0)
#endif

static INLINE float fminf_internal(float a, float b)
{
  return a < b ? a : b;
}

static INLINE float fmaxf_internal(float a, float b)
{
  return a > b ? a : b;
}

static double round_internal(double x)
{
   double t;

#ifdef _MSC_VER
   if (!_finite(x))
#else
   if (!isfinite(x))
#endif
      return x;

   if (x >= 0.0) {
      t = floor(x);
      if (t - x <= -0.5)
         t += 1.0;
      return t;
   } else {
      t = floor(-x);
      if (t + x <= -0.5)
         t += 1.0;
      return -t;
   }
}

#ifdef TEST_FPS
static double frames = 200.0f;

double sglfwGetTime(void)
{
   double val = frames;
   return val;
}

void sglfwSetTime(double val)
{
}

#define glfwGetTime sglfwGetTime
#define glfwSetTime sglfwSetTime
#endif

static Model model;

static int rand_int(int n)
{
    int result;
    while (n <= (result = rand() / (RAND_MAX / n)));
    return result;
}

static double rand_double() {
    return (double)rand() / (double)RAND_MAX;
}

static void update_fps(FPS *fps)
{
   double now, elapsed;
   fps->frames++;
   now     = glfwGetTime();
   elapsed = now - fps->since;

   if (elapsed >= 1)
   {
      fps->fps = round_internal(fps->frames / elapsed);
      fps->frames = 0;
      fps->since = now;
   }
}

static float *malloc_faces(int components, int faces)
{
    return (float*)malloc(sizeof(float) *  6 * components * faces);
}

static void flip_image_vertical(
    unsigned char *data, unsigned int width, unsigned int height)
{
   unsigned int i;
   unsigned int size       = width * height * 4;
   unsigned int stride     = sizeof(int8_t) * width * 4;
   unsigned char *new_data = malloc(sizeof(unsigned char) * size);

   for (i = 0; i < height; i++)
   {
      unsigned int j = height - i - 1;
      memcpy(new_data + j * stride, data + i * stride, stride);
   }
   memcpy(data, new_data, size);
   renderer_upload_image(width, height, data);
   free(new_data);
}

static void load_png_texture_data(const unsigned char *in_data, size_t in_size)
{
    unsigned char *data;
    unsigned int width, height;
    unsigned int error = lodepng_decode32(&data, &width,
          &height, in_data, in_size);

    if (error)
        fprintf(stderr, "error %u: %s\n", error, lodepng_error_text(error));
    flip_image_vertical(data, width, height);
    free(data);
}

static char *tokenize(char *str, const char *delim, char **key)
{
    char *result;
    if (str == NULL)
        str = *key;
    str += strspn(str, delim);
    if (*str == '\0')
        return NULL;
    result = str;
    str += strcspn(str, delim);
    if (*str)
        *str++ = '\0';
    *key = str;
    return result;
}

static int char_width(char input)
{
    static const int lookup[128] = {
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        4, 2, 4, 7, 6, 9, 7, 2, 3, 3, 4, 6, 3, 5, 2, 7,
        6, 3, 6, 6, 6, 6, 6, 6, 6, 6, 2, 3, 5, 6, 5, 7,
        8, 6, 6, 6, 6, 6, 6, 6, 6, 4, 6, 6, 5, 8, 8, 6,
        6, 7, 6, 6, 6, 6, 8,10, 8, 6, 6, 3, 6, 3, 6, 6,
        4, 7, 6, 6, 6, 6, 5, 6, 6, 2, 5, 5, 2, 9, 6, 6,
        6, 6, 6, 6, 5, 6, 6, 6, 6, 6, 6, 4, 2, 5, 7, 0
    };
    return lookup[input];
}

static int string_width(const char *input)
{
   unsigned i;
   int result = 0;
   int length = strlen(input);

   for (i = 0; i < length; i++)
      result += char_width(input[i]);

   return result;
}

static int wrap(const char *input, int max_width, char *output, int max_length)
{
   char *line = NULL;
   char *text = NULL;
   char *key1, *key2;
   int space_width;
   int line_number = 0;

   *output = '\0';
   text = malloc(sizeof(char) * (strlen(input) + 1));
   strcpy(text, input);
   space_width = char_width(' ');
   line = tokenize(text, "\r\n", &key1);

   while (line)
   {
      int line_width = 0;
      char *token = tokenize(line, " ", &key2);
      while (token)
      {
         int token_width = string_width(token);
         if (line_width)
         {
            if (line_width + token_width > max_width)
            {
               line_width = 0;
               line_number++;
               strncat(output, "\n", max_length - strlen(output) - 1);
            }
            else
               strncat(output, " ", max_length - strlen(output) - 1);
         }
         strncat(output, token, max_length - strlen(output) - 1);
         line_width += token_width + space_width;
         token = tokenize(NULL, " ", &key2);
      }
      line_number++;
      strncat(output, "\n", max_length - strlen(output) - 1);
      line = tokenize(NULL, "\r\n", &key1);
   }
   free(text);
   return line_number;
}

static int chunked(float x)
{
    return floorf(roundf(x) / CHUNK_SIZE);
}

static float time_of_day()
{
   float t;
   Model *g = (Model*)&model;

   if (g->day_length <= 0)
      return 0.5;
   t = glfwGetTime();
   t = t / g->day_length;
   t = t - (int)t;
   return t;
}

static float get_daylight()
{
   float t;
   float timer = time_of_day();
   if (timer < 0.5)
   {
      t = (timer - 0.25) * 100;
      return 1 / (1 + powf(2, -t));
   }

   t = (timer - 0.85) * 100;
   return 1 - 1 / (1 + powf(2, -t));
}

static int get_scale_factor(void)
{
   int result;
   int window_width, window_height;
   int buffer_width, buffer_height;
   window_width  = buffer_width  = game_width;
   window_height = buffer_height = game_height;
   result = buffer_width / window_width;
   result = MAX(1, result);
   result = MIN(2, result);

   return result;
}

static void get_sight_vector(float rx, float ry, float *vx, float *vy, float *vz)
{
    float m = cosf(ry);
    *vx = cosf(rx - RADIANS(90)) * m;
    *vy = sinf(ry);
    *vz = sinf(rx - RADIANS(90)) * m;
}

static void get_motion_vector(int flying, float sz, float sx, float rx, float ry,
    float *vx, float *vy, float *vz)
{
   float strafe;
    *vx = 0.0; *vy = 0.0; *vz = 0.0;
    if (!sz && !sx)
        return;

    strafe = atan2f(sz, sx);

    if (flying)
    {
        float m = cosf(ry);
        float y = sinf(ry);
        if (sx)
        {
            if (!sz)
                y = 0;
            m = 1;
        }
        if (sz > 0.0)
            y = -y;
        *vx = cosf(rx + strafe) * m;
        *vy = y;
        *vz = sinf(rx + strafe) * m;
    }
    else
    {
        *vx = cosf(rx + strafe) * sqrt(sz*sz + sx*sx);
        *vy = 0;
        *vz = sinf(rx + strafe) * sqrt(sz*sz + sx*sx);
    }
}

static uintptr_t gen_crosshair_buffer(void)
{
   Model *g = (Model*)&model;
   int x = g->width / 2;
   int y = g->height / 2;
   int p = 10 * g->scale;
   float data[] = {
      x, y - p, x, y + p,
      x - p, y, x + p, y
   };
   return renderer_gen_buffer(sizeof(data), data);
}

static uintptr_t gen_wireframe_buffer(float x, float y, float z, float n)
{
    float data[72];
    make_cube_wireframe(data, x, y, z, n);
    return renderer_gen_buffer(sizeof(data), data);
}

static uintptr_t gen_water_buffer(float x, float y, float z, float n)
{
    float data[120];
    float ao[6][4] = {0};
    float light[6][4] = {
        {0.5, 0.5, 0.5, 0.5},
        {0.5, 0.5, 0.5, 0.5},
        {0.5, 0.5, 0.5, 0.5},
        {0.5, 0.5, 0.5, 0.5},
        {0.5, 0.5, 0.5, 0.5},
        {0.5, 0.5, 0.5, 0.5}
    };
    make_cube_faces(
        data, ao, light,
        0, 0, 1, 0, 0, 0,
        0, 0, 255, 0, 0, 0,
        x, y - n, z, n);
    make_cube_faces(
        data + 60, ao, light,
        0, 0, 0, 1, 0, 0,
        0, 0, 0, 255, 0, 0,
        x, y + n, z, n);
    return renderer_gen_buffer(sizeof(data), data);
}

static uintptr_t gen_sky_buffer(void)
{
   float data[12288];
   make_sphere(data, 1, 3);
   return renderer_gen_buffer(sizeof(data), data);
}

static uintptr_t gen_cube_buffer(float x, float y, float z, float n, int w)
{
    float *data = malloc_faces(10, 6);
    float ao[6][4] = {0};
    float light[6][4] = {
        {0.5, 0.5, 0.5, 0.5},
        {0.5, 0.5, 0.5, 0.5},
        {0.5, 0.5, 0.5, 0.5},
        {0.5, 0.5, 0.5, 0.5},
        {0.5, 0.5, 0.5, 0.5},
        {0.5, 0.5, 0.5, 0.5}
    };
    make_cube(data, ao, light, 1, 1, 1, 1, 1, 1, x, y, z, n, w);
    return renderer_gen_faces(10, 6, data);
}

static uintptr_t gen_plant_buffer(float x, float y, float z, float n, int w)
{
    float *data = malloc_faces(10, 4);
    float ao    = 0;
    float light = 1;

    make_plant(data, ao, light, x, y, z, n, w, 45);
    return renderer_gen_faces(10, 4, data);
}

static uintptr_t gen_player_buffer(float x, float y, float z, float rx, float ry)
{
    float *data = malloc_faces(10, 6);
    make_player(data, x, y, z, rx, ry);
    return renderer_gen_faces(10, 6, data);
}

static uintptr_t gen_text_buffer(float x, float y, float n, char *text)
{
   unsigned i;
   int length  = strlen(text);
   float *data = malloc_faces(4, length);
   for (i = 0; i < length; i++)
   {
      make_character(data + i * 24, x, y, n / 2, n, text[i]);
      x += n;
   }
   return renderer_gen_faces(4, length, data);
}

static void draw_triangles_3d_ao(Attrib *attrib, uintptr_t buffer, int count) {
   unsigned attrib_size   = 3;
   unsigned normal_enable = 1;
   unsigned uv_enable     = 1;

   renderer_bind_array_buffer(attrib, buffer, normal_enable, uv_enable);
   renderer_modify_array_buffer(attrib, attrib_size, normal_enable, uv_enable, 10);
   renderer_draw_triangle_arrays(DRAW_PRIM_TRIANGLES, count);
   renderer_unbind_array_buffer(attrib, normal_enable, uv_enable);
}

static void draw_triangles_3d_text(Attrib *attrib, uintptr_t buffer, int count) {
   unsigned attrib_size   = 3;
   unsigned normal_enable = 0;
   unsigned uv_enable     = 1;

   renderer_bind_array_buffer(attrib, buffer, normal_enable, uv_enable);
   renderer_modify_array_buffer(attrib, attrib_size, normal_enable, uv_enable, 5);
   renderer_draw_triangle_arrays(DRAW_PRIM_TRIANGLES, count);
   renderer_unbind_array_buffer(attrib, normal_enable, uv_enable);
}

static void draw_triangles_3d(Attrib *attrib, uintptr_t buffer, int count) {
   unsigned attrib_size   = 3;
   unsigned normal_enable = 1;
   unsigned uv_enable     = 1;

   renderer_bind_array_buffer(attrib, buffer, normal_enable, uv_enable);
   renderer_modify_array_buffer(attrib, attrib_size, normal_enable, uv_enable, 8);
   renderer_draw_triangle_arrays(DRAW_PRIM_TRIANGLES, count);
   renderer_unbind_array_buffer(attrib, normal_enable, uv_enable);
}

static void draw_triangles_2d(Attrib *attrib, uintptr_t buffer, int count) {
   unsigned attrib_size   = 2;
   unsigned normal_enable = 0;
   unsigned uv_enable     = 1;

   renderer_bind_array_buffer(attrib, buffer, normal_enable, uv_enable);
   renderer_modify_array_buffer(attrib, attrib_size, normal_enable, uv_enable, 4);
   renderer_draw_triangle_arrays(DRAW_PRIM_TRIANGLES, count);
   renderer_unbind_array_buffer(attrib, normal_enable, uv_enable);
}

static void draw_lines(Attrib *attrib, uintptr_t buffer, int components, int count)
{
   unsigned normal_enable = 0;
   unsigned uv_enable     = 0;

   renderer_bind_array_buffer(attrib, buffer, normal_enable, uv_enable);
   renderer_modify_array_buffer(attrib, components, 0, 0, 0);
   renderer_draw_triangle_arrays(DRAW_PRIM_LINES, count);
   renderer_unbind_array_buffer(attrib, normal_enable, uv_enable);
}

static Player *find_player(int id)
{
   unsigned i;
   Model *g = (Model*)&model;

   for (i = 0; i < g->player_count; i++)
   {
      Player *player = g->players + i;
      if (player->id == id)
         return player;
   }
   return 0;
}

static void update_player(Player *player,
    float x, float y, float z, float rx, float ry, int interpolate)
{
    if (interpolate) {
        State *s1 = &player->state1;
        State *s2 = &player->state2;
        memcpy(s1, s2, sizeof(State));
        s2->x = x; s2->y = y; s2->z = z; s2->rx = rx; s2->ry = ry;
        s2->t = glfwGetTime();
        if (s2->rx - s1->rx > PI)
            s1->rx += 2 * PI;
        if (s1->rx - s2->rx > PI)
            s1->rx -= 2 * PI;
    }
    else
    {
        State *s = &player->state;
        s->x = x; s->y = y; s->z = z; s->rx = rx; s->ry = ry;
        renderer_del_buffer(player->buffer);
        player->buffer = gen_player_buffer(s->x, s->y, s->z, s->rx, s->ry);
    }
}

static void interpolate_player(Player *player)
{
   float p, t1, t2;
   State *s1 = &player->state1;
   State *s2 = &player->state2;

   t1 = s2->t - s1->t;
   t2 = glfwGetTime() - s2->t;
   t1 = MIN(t1, 1);
   t1 = MAX(t1, 0.1);
   p = MIN(t2 / t1, 1);
   update_player(
         player,
         s1->x + (s2->x - s1->x) * p,
         s1->y + (s2->y - s1->y) * p,
         s1->z + (s2->z - s1->z) * p,
         s1->rx + (s2->rx - s1->rx) * p,
         s1->ry + (s2->ry - s1->ry) * p,
         0);
}

static void delete_player(int id)
{
   int count;
   Player *other;
   Player *player = find_player(id);
   Model *g = (Model*)&model;
   if (!player)
      return;
   count = g->player_count;
   renderer_del_buffer(player->buffer);
   other = g->players + (--count);
   memcpy(player, other, sizeof(Player));
   g->player_count = count;
}

static void delete_all_players()
{
   int i;
   Model *g = (Model*)&model;
   for (i = 0; i < g->player_count; i++)
   {
      Player *player = g->players + i;
      renderer_del_buffer(player->buffer);
   }
   g->player_count = 0;
}

static float player_player_distance(Player *p1, Player *p2) {
    State *s1 = &p1->state;
    State *s2 = &p2->state;
    float x = s2->x - s1->x;
    float y = s2->y - s1->y;
    float z = s2->z - s1->z;
    return sqrtf(x * x + y * y + z * z);
}

static float player_crosshair_distance(Player *p1, Player *p2) {
    float px, py, pz;
    float vx, vy, vz;
    float x, y, z;
    State *s1 = &p1->state;
    State *s2 = &p2->state;
    float d = player_player_distance(p1, p2);
    get_sight_vector(s1->rx, s1->ry, &vx, &vy, &vz);
    vx *= d; vy *= d; vz *= d;
    px = s1->x + vx; py = s1->y + vy; pz = s1->z + vz;
    x = s2->x - px;
    y = s2->y - py;
    z = s2->z - pz;
    return sqrtf(x * x + y * y + z * z);
}

static Player *player_crosshair(Player *player)
{
   int i;
   Player *result = 0;
   float threshold = RADIANS(5);
   float best = 0;
   Model *g = (Model*)&model;
   for (i = 0; i < g->player_count; i++)
   {
      float p, d;
      Player *other = g->players + i;
      if (other == player)
         continue;
      p = player_crosshair_distance(player, other);
      d = player_player_distance(player, other);
      if (d < 96 && p / d < threshold) {
         if (best == 0 || d < best) {
            best = d;
            result = other;
         }
      }
   }
   return result;
}

static Chunk *find_chunk(int p, int q)
{
   unsigned i;
   Model *g = (Model*)&model;
   for (i = 0; i < g->chunk_count; i++)
   {
      Chunk *chunk = g->chunks + i;
      if (chunk->p == p && chunk->q == q)
         return chunk;
   }
   return 0;
}

static int chunk_distance(Chunk *chunk, int p, int q) {
    int dp = ABS(chunk->p - p);
    int dq = ABS(chunk->q - q);
    return MAX(dp, dq);
}

static int chunk_visible(float planes[6][4], int p, int q, int miny, int maxy)
{
   unsigned i, j;
   int x = p * CHUNK_SIZE - 1;
   int z = q * CHUNK_SIZE - 1;
   int d = CHUNK_SIZE + 1;
   Model *g = (Model*)&model;
   float points[8][3] = {
      {x + 0, miny, z + 0},
      {x + d, miny, z + 0},
      {x + 0, miny, z + d},
      {x + d, miny, z + d},
      {x + 0, maxy, z + 0},
      {x + d, maxy, z + 0},
      {x + 0, maxy, z + d},
      {x + d, maxy, z + d}
   };
   int n = g->ortho ? 4 : 6;
   for (i = 0; i < n; i++)
   {
      int in = 0;
      int out = 0;

      for (j = 0; j < 8; j++)
      {
         float d =
            planes[i][0] * points[j][0] +
            planes[i][1] * points[j][1] +
            planes[i][2] * points[j][2] +
            planes[i][3];
         if (d < 0)
            out++;
         else
            in++;
         if (in && out)
            break;
      }

      if (in == 0)
         return 0;
   }
   return 1;
}

static int highest_block(float x, float z)
{
    int result = -1;
    int nx = roundf(x);
    int nz = roundf(z);
    int p = chunked(x);
    int q = chunked(z);
    Chunk *chunk = find_chunk(p, q);
    if (chunk) {
        Map *map = &chunk->map;
        MAP_FOR_EACH(map, ex, ey, ez, ew)
        {
            if (is_obstacle(ew) && ex == nx && ez == nz)
                result = MAX(result, ey);
        } END_MAP_FOR_EACH;
    }
    return result;
}

static int _hit_test(
    Map *map, float max_distance, int previous,
    float x, float y, float z,
    float vx, float vy, float vz,
    int *hx, int *hy, int *hz)
{
    int m = 32;
    int px = 0;
    int py = 0;
    int pz = 0;
    unsigned i;
    for (i = 0; i < max_distance * m; i++)
    {
       int nx = roundf(x);
       int ny = roundf(y);
       int nz = roundf(z);
       if (nx != px || ny != py || nz != pz)
       {
          int hw = map_get(map, nx, ny, nz);
          if (hw > 0)
          {
             if (previous)
             {
                *hx = px; *hy = py; *hz = pz;
             }
             else
             {
                *hx = nx; *hy = ny; *hz = nz;
             }
             return hw;
          }
          px = nx; py = ny; pz = nz;
       }
       x += vx / m; y += vy / m; z += vz / m;
    }
    return 0;
}

static int hit_test(
    int previous, float x, float y, float z, float rx, float ry,
    int *bx, int *by, int *bz)
{
   unsigned i;
   float vx, vy, vz;
   int result = 0;
   float best = 0;
   int p      = chunked(x);
   int q      = chunked(z);
   Model *g = (Model*)&model;

   get_sight_vector(rx, ry, &vx, &vy, &vz);

   for (i = 0; i < g->chunk_count; i++)
   {
      int hx, hy, hz, hw;
      Chunk *chunk = g->chunks + i;
      if (chunk_distance(chunk, p, q) > 1)
         continue;
      hw = _hit_test(&chunk->map, 8, previous,
            x, y, z, vx, vy, vz, &hx, &hy, &hz);
      if (hw > 0)
      {
         float d = sqrtf(
               powf(hx - x, 2) + powf(hy - y, 2) + powf(hz - z, 2));
         if (best == 0 || d < best)
         {
            best = d;
            *bx = hx; *by = hy; *bz = hz;
            result = hw;
         }
      }
   }
   return result;
}

static int hit_test_face(Player *player, int *x, int *y, int *z, int *face) {
    State *s = &player->state;
    int w = hit_test(0, s->x, s->y, s->z, s->rx, s->ry, x, y, z);
    if (is_obstacle(w)) {
        int hx, hy, hz, dx, dy, dz;
        hit_test(1, s->x, s->y, s->z, s->rx, s->ry, &hx, &hy, &hz);
        dx = hx - *x;
        dy = hy - *y;
        dz = hz - *z;
        if (dx == -1 && dy == 0 && dz == 0) {
            *face = 0; return 1;
        }
        if (dx == 1 && dy == 0 && dz == 0) {
            *face = 1; return 1;
        }
        if (dx == 0 && dy == 0 && dz == -1) {
            *face = 2; return 1;
        }
        if (dx == 0 && dy == 0 && dz == 1) {
            *face = 3; return 1;
        }
        if (dx == 0 && dy == 1 && dz == 0) {
           int top;
            int degrees = roundf(DEGREES(atan2f(s->x - hx, s->z - hz)));
            if (degrees < 0) {
                degrees += 360;
            }
            top = ((degrees + 45) / 90) % 4;
            *face = 4 + top; return 1;
        }
    }
    return 0;
}

static int collide(int height, float *x, float *y, float *z) {
    int result = 0;
    int p = chunked(*x);
    int q = chunked(*z);
    Chunk *chunk = find_chunk(p, q);
    if (!chunk)
        return result;
    {
       int dy;
       Map *map = &chunk->map;
       int nx = roundf(*x);
       int ny = roundf(*y);
       int nz = roundf(*z);
       float px = *x - nx;
       float py = *y - ny;
       float pz = *z - nz;
       float pad = 0.25;
       for (dy = 0; dy < height; dy++) {
          if (px < -pad && is_obstacle(map_get(map, nx - 1, ny - dy, nz)))
             *x = nx - pad;
          if (px > pad && is_obstacle(map_get(map, nx + 1, ny - dy, nz)))
             *x = nx + pad;
          if (py < -pad && is_obstacle(map_get(map, nx, ny - dy - 1, nz)))
          {
             *y = ny - pad;
             result = 1;
          }
          if (py > pad && is_obstacle(map_get(map, nx, ny - dy + 1, nz)))
          {
             *y = ny + pad;
             result = 1;
          }
          if (pz < -pad && is_obstacle(map_get(map, nx, ny - dy, nz - 1)))
             *z = nz - pad;
          if (pz > pad && is_obstacle(map_get(map, nx, ny - dy, nz + 1)))
             *z = nz + pad;
       }
    }
    return result;
}

static int player_intersects_block(
    int height,
    float x, float y, float z,
    int hx, int hy, int hz)
{
   unsigned i;
   int nx = roundf(x);
   int ny = roundf(y);
   int nz = roundf(z);
   for (i = 0; i < height; i++)
   {
      if (nx == hx && ny - i == hy && nz == hz)
         return 1;
   }
   return 0;
}

static int _gen_sign_buffer(
    float *data, float x, float y, float z, int face, const char *text)
{
   int count = 0;
    static const int glyph_dx[8] = {0, 0, -1, 1, 1, 0, -1, 0};
    static const int glyph_dz[8] = {1, -1, 0, 0, 0, -1, 0, 1};
    static const int line_dx[8] = {0, 0, 0, 0, 0, 1, 0, -1};
    static const int line_dy[8] = {-1, -1, -1, -1, 0, 0, 0, 0};
    static const int line_dz[8] = {0, 0, 0, 0, 1, 0, -1, 0};
    if (face < 0 || face >= 8) {
        return 0;
    }
    {
       char *key;
       char *line;
       float max_width = 64;
       float line_height = 1.25;
       int8_t lines[1024];
       int rows = wrap(text, max_width, lines, 1024);
       int dx = glyph_dx[face];
       int dz = glyph_dz[face];
       int ldx = line_dx[face];
       int ldy = line_dy[face];
       int ldz = line_dz[face];
       float n = 1.0 / (max_width / 10);
       float sx, sy, sz;

       rows = MIN(rows, 5);
       sx = x - n * (rows - 1) * (line_height / 2) * ldx;
       sy = y - n * (rows - 1) * (line_height / 2) * ldy;
       sz = z - n * (rows - 1) * (line_height / 2) * ldz;
       line = tokenize(lines, "\n", &key);
       while (line) {
          int i;
          float rx, ry, rz;
          int length = strlen(line);
          int line_width = string_width(line);
          line_width = MIN(line_width, max_width);
          rx = sx - dx * line_width / max_width / 2;
          ry = sy;
          rz = sz - dz * line_width / max_width / 2;

          for (i = 0; i < length; i++) {
             int width = char_width(line[i]);
             line_width -= width;
             if (line_width < 0)
                break;
             rx += dx * width / max_width / 2;
             rz += dz * width / max_width / 2;
             if (line[i] != ' ')
             {
                make_character_3d(
                      data + count * 30, rx, ry, rz, n / 2, face, line[i]);
                count++;
             }
             rx += dx * width / max_width / 2;
             rz += dz * width / max_width / 2;
          }
          sx += n * line_height * ldx;
          sy += n * line_height * ldy;
          sz += n * line_height * ldz;
          line = tokenize(NULL, "\n", &key);
          rows--;
          if (rows <= 0) {
             break;
          }
       }
    }
    return count;
}

static void gen_sign_buffer(Chunk *chunk)
{
   unsigned i;
   int faces       = 0;
   float *data     = NULL;
   SignList *signs = &chunk->signs;
   // first pass - count characters
   int max_faces = 0;

   for (i = 0; i < signs->size; i++)
   {
      Sign *e = signs->data + i;
      max_faces += strlen(e->text);
   }

   // second pass - generate geometry
   data = malloc_faces(5, max_faces);

   for (i = 0; i < signs->size; i++)
   {
      Sign *e = signs->data + i;
      faces += _gen_sign_buffer(
            data + faces * 30, e->x, e->y, e->z, e->face, e->text);
   }

   renderer_del_buffer(chunk->sign_buffer);
   chunk->sign_buffer = renderer_gen_faces(5, faces, data);
   chunk->sign_faces  = faces;
}

static int has_lights(Chunk *chunk)
{
   int dp, dq;
   if (!SHOW_LIGHTS)
      return 0;

   for (dp = -1; dp <= 1; dp++)
   {
      for (dq = -1; dq <= 1; dq++)
      {
         Map *map;
         Chunk *other = chunk;
         if (dp || dq)
            other = find_chunk(chunk->p + dp, chunk->q + dq);
         if (!other)
            continue;
         map = &other->lights;
         if (map->size)
            return 1;
      }
   }

   return 0;
}

static void dirty_chunk(Chunk *chunk)
{
   int dp, dq;

   chunk->dirty = 1;

   if (!has_lights(chunk))
      return;

   for (dp = -1; dp <= 1; dp++)
   {
      for (dq = -1; dq <= 1; dq++)
      {
         Chunk *other = find_chunk(chunk->p + dp, chunk->q + dq);
         if (other)
            other->dirty = 1;
      }
   }
}

static void occlusion(
    int8_t neighbors[27], int8_t lights[27], float shades[27],
    float ao[6][4], float light[6][4])
{
   unsigned i, j;
    static const int lookup3[6][4][3] = {
        {{0, 1, 3}, {2, 1, 5}, {6, 3, 7}, {8, 5, 7}},
        {{18, 19, 21}, {20, 19, 23}, {24, 21, 25}, {26, 23, 25}},
        {{6, 7, 15}, {8, 7, 17}, {24, 15, 25}, {26, 17, 25}},
        {{0, 1, 9}, {2, 1, 11}, {18, 9, 19}, {20, 11, 19}},
        {{0, 3, 9}, {6, 3, 15}, {18, 9, 21}, {24, 15, 21}},
        {{2, 5, 11}, {8, 5, 17}, {20, 11, 23}, {26, 17, 23}}
    };
   static const int lookup4[6][4][4] = {
        {{0, 1, 3, 4}, {1, 2, 4, 5}, {3, 4, 6, 7}, {4, 5, 7, 8}},
        {{18, 19, 21, 22}, {19, 20, 22, 23}, {21, 22, 24, 25}, {22, 23, 25, 26}},
        {{6, 7, 15, 16}, {7, 8, 16, 17}, {15, 16, 24, 25}, {16, 17, 25, 26}},
        {{0, 1, 9, 10}, {1, 2, 10, 11}, {9, 10, 18, 19}, {10, 11, 19, 20}},
        {{0, 3, 9, 12}, {3, 6, 12, 15}, {9, 12, 18, 21}, {12, 15, 21, 24}},
        {{2, 5, 11, 14}, {5, 8, 14, 17}, {11, 14, 20, 23}, {14, 17, 23, 26}}
    };
    static const float curve[4] = {0.0, 0.25, 0.5, 0.75};

    for (i = 0; i < 6; i++)
    {
        for (j = 0; j < 4; j++)
        {
           float total;
           unsigned k;
           int corner = neighbors[lookup3[i][j][0]];
           int side1 = neighbors[lookup3[i][j][1]];
           int side2 = neighbors[lookup3[i][j][2]];
           int value = side1 && side2 ? 3 : corner + side1 + side2;
           float shade_sum = 0;
           float light_sum = 0;
           int is_light = lights[13] == 15;

           for (k = 0; k < 4; k++) {
              shade_sum += shades[lookup4[i][j][k]];
              light_sum += lights[lookup4[i][j][k]];
           }
           if (is_light)
              light_sum = 15 * 4 * 10;
           total = curve[value] + shade_sum / 4.0;
           ao[i][j] = MIN(total, 1.0);
           light[i][j] = light_sum / 15.0 / 4.0;
        }
    }
}

#define XZ_SIZE (CHUNK_SIZE * 3 + 2)
#define XZ_LO (CHUNK_SIZE)
#define XZ_HI (CHUNK_SIZE * 2 + 1)
#define Y_SIZE (MAX_BLOCK_HEIGHT + 2)
#define XYZ(x, y, z) ((y) * XZ_SIZE * XZ_SIZE + (x) * XZ_SIZE + (z))
#define XZ(x, z) ((x) * XZ_SIZE + (z))

static void light_fill(
    int8_t *opaque, int8_t *light,
    int x, int y, int z, int w, int force)
{
    if (x + w < XZ_LO || z + w < XZ_LO)
        return;
    if (x - w > XZ_HI || z - w > XZ_HI)
        return;
    if (y < 0 || y >= Y_SIZE)
        return;
    if (light[XYZ(x, y, z)] >= w)
        return;
    if (!force && opaque[XYZ(x, y, z)])
        return;

    light[XYZ(x, y, z)] = w--;
    light_fill(opaque, light, x - 1, y, z, w, 0);
    light_fill(opaque, light, x + 1, y, z, w, 0);
    light_fill(opaque, light, x, y - 1, z, w, 0);
    light_fill(opaque, light, x, y + 1, z, w, 0);
    light_fill(opaque, light, x, y, z - 1, w, 0);
    light_fill(opaque, light, x, y, z + 1, w, 0);
}

static void compute_chunk(WorkerItem *item)
{
   Map *map;
   unsigned a, b;
   int miny = MAX_BLOCK_HEIGHT;
   int maxy = 0;
   int faces = 0;
   int8_t *opaque  = (int8_t *)calloc(XZ_SIZE * XZ_SIZE * Y_SIZE, sizeof(int8_t));
   int8_t *light   = (int8_t*)calloc(XZ_SIZE * XZ_SIZE * Y_SIZE, sizeof(int8_t));
   int8_t *highest = (int8_t*)calloc(XZ_SIZE * XZ_SIZE, sizeof(int8_t));
   int ox        = item->p * CHUNK_SIZE - CHUNK_SIZE - 1;
   int oy        = -1;
   int oz        = item->q * CHUNK_SIZE - CHUNK_SIZE - 1;
   /* check for lights */
   int has_light = 0;
   if (SHOW_LIGHTS)
   {
      for (a = 0; a < 3; a++)
      {
         for (b = 0; b < 3; b++)
         {
            Map *map = item->light_maps[a][b];
            if (map && map->size)
               has_light = 1;
         }
      }
   }

   // populate opaque array
   for (a = 0; a < 3; a++)
   {
      for (b = 0; b < 3; b++)
      {
         Map *map = item->block_maps[a][b];
         if (!map)
            continue;
         MAP_FOR_EACH(map, ex, ey, ez, ew)
         {
            int x = ex - ox;
            int y = ey - oy;
            int z = ez - oz;
            int w = ew;
            // TODO: this should be unnecessary
            if (x < 0 || y < 0 || z < 0)
               continue;
            if (x >= XZ_SIZE || y >= Y_SIZE || z >= XZ_SIZE)
               continue;
            // END TODO
            opaque[XYZ(x, y, z)] = !is_transparent(w);
            if (opaque[XYZ(x, y, z)])
               highest[XZ(x, z)] = MAX(highest[XZ(x, z)], y);
         } END_MAP_FOR_EACH;
      }
   }

   // flood fill light intensities
   if (has_light)
   {
      for (a = 0; a < 3; a++)
      {
         for (b = 0; b < 3; b++)
         {
            Map *map = item->light_maps[a][b];
            if (!map)
               continue;
            MAP_FOR_EACH(map, ex, ey, ez, ew)
            {
               int x = ex - ox;
               int y = ey - oy;
               int z = ez - oz;
               light_fill(opaque, light, x, y, z, ew, 1);
            } END_MAP_FOR_EACH;
         }
      }
   }

   map = item->block_maps[1][1];

   /* count exposed faces */
   MAP_FOR_EACH(map, ex, ey, ez, ew) {
      if (ew <= 0)
         continue;
      {
         int x = ex - ox;
         int y = ey - oy;
         int z = ez - oz;
         int f1 = !opaque[XYZ(x - 1, y, z)];
         int f2 = !opaque[XYZ(x + 1, y, z)];
         int f3 = !opaque[XYZ(x, y + 1, z)];
         int f4 = !opaque[XYZ(x, y - 1, z)] && (ey > 0);
         int f5 = !opaque[XYZ(x, y, z - 1)];
         int f6 = !opaque[XYZ(x, y, z + 1)];
         int total = f1 + f2 + f3 + f4 + f5 + f6;
         if (total == 0)
            continue;
         if (is_plant(ew))
            total = 4;
         miny = MIN(miny, ey);
         maxy = MAX(maxy, ey);
         faces += total;
      }
   } END_MAP_FOR_EACH;

   {
      // generate geometry
      float *data = malloc_faces(10, faces);
      int offset = 0;
      MAP_FOR_EACH(map, ex, ey, ez, ew) {
         int8_t neighbors[27] = {0};
         int8_t lights[27] = {0};
         float shades[27] = {0};
         int index = 0;
         int dx;
         int x = ex - ox;
         int y = ey - oy;
         int z = ez - oz;
         int f1 = !opaque[XYZ(x - 1, y, z)];
         int f2 = !opaque[XYZ(x + 1, y, z)];
         int f3 = !opaque[XYZ(x, y + 1, z)];
         int f4 = !opaque[XYZ(x, y - 1, z)] && (ey > 0);
         int f5 = !opaque[XYZ(x, y, z - 1)];
         int f6 = !opaque[XYZ(x, y, z + 1)];
         int total = f1 + f2 + f3 + f4 + f5 + f6;
         if (ew <= 0) {
            continue;
         }
         if (total == 0) {
            continue;
         }
         for (dx = -1; dx <= 1; dx++) {
            int dy;
            for (dy = -1; dy <= 1; dy++) {
               int dz;
               for (dz = -1; dz <= 1; dz++) {
                  neighbors[index] = opaque[XYZ(x + dx, y + dy, z + dz)];
                  lights[index] = light[XYZ(x + dx, y + dy, z + dz)];
                  shades[index] = 0;
                  if (y + dy <= highest[XZ(x + dx, z + dz)]) {
                     int oy;
                     for (oy = 0; oy < 8; oy++) {
                        if (opaque[XYZ(x + dx, y + dy + oy, z + dz)]) {
                           shades[index] = 1.0 - oy * 0.125;
                           break;
                        }
                     }
                  }
                  index++;
               }
            }
         }

         {
            float ao[6][4];
            float light[6][4];
            occlusion(neighbors, lights, shades, ao, light);

            if (is_plant(ew))
            {
               int a;
               float rotation;
               float min_ao = 1;
               float max_light = 0;
               total = 4;
               for (a = 0; a < 6; a++)
               {
                  int b;
                  for (b = 0; b < 4; b++)
                  {
                     min_ao = MIN(min_ao, ao[a][b]);
                     max_light = MAX(max_light, light[a][b]);
                  }
               }
               rotation = simplex2(ex, ez, 4, 0.5, 2) * 360;
               make_plant(
                     data + offset, min_ao, max_light,
                     ex, ey, ez, 0.5, ew, rotation);
            }
            else
               make_cube(
                     data + offset, ao, light,
                     f1, f2, f3, f4, f5, f6,
                     ex, ey, ez, 0.5, ew);
            offset += total * 60;
         }
      } END_MAP_FOR_EACH;

      free(opaque);
      free(light);
      free(highest);

      item->miny = miny;
      item->maxy = maxy;
      item->faces = faces;
      item->data = data;
   }
}

static void generate_chunk(Chunk *chunk, WorkerItem *item) {
    chunk->miny = item->miny;
    chunk->maxy = item->maxy;
    chunk->faces = item->faces;
    renderer_del_buffer(chunk->buffer);
    chunk->buffer = renderer_gen_faces(10, item->faces, item->data);
    gen_sign_buffer(chunk);
}

static void gen_chunk_buffer(Chunk *chunk)
{
   int dp;
   WorkerItem _item;
   WorkerItem *item = &_item;

   item->p = chunk->p;
   item->q = chunk->q;

   for (dp = -1; dp <= 1; dp++)
   {
      int dq;
      for (dq = -1; dq <= 1; dq++)
      {
         Chunk *other = chunk;
         if (dp || dq)
            other = find_chunk(chunk->p + dp, chunk->q + dq);

         if (other)
         {
            item->block_maps[dp + 1][dq + 1] = &other->map;
            item->light_maps[dp + 1][dq + 1] = &other->lights;
         }
         else
         {
            item->block_maps[dp + 1][dq + 1] = 0;
            item->light_maps[dp + 1][dq + 1] = 0;
         }
      }
   }
   compute_chunk(item);
   generate_chunk(chunk, item);
   chunk->dirty = 0;
}

static void map_set_func(int x, int y, int z, int w, void *arg)
{
    Map *map = (Map *)arg;
    map_set(map, x, y, z, w);
}

static void load_chunk(WorkerItem *item)
{
    int p = item->p;
    int q = item->q;
    Map *block_map = item->block_maps[1][1];
    Map *light_map = item->light_maps[1][1];
    create_world(p, q, map_set_func, block_map);
    db_load_blocks(block_map, p, q);
    db_load_lights(light_map, p, q);
}

static void request_chunk(int p, int q)
{
   int key = db_get_key(p, q);
   client_chunk(p, q, key);
}

static void init_chunk(Chunk *chunk, int p, int q)
{
   int dx, dy, dz;
   Map *block_map;
   Map *light_map;
   SignList *signs;

   chunk->p = p;
   chunk->q = q;
   chunk->faces = 0;
   chunk->sign_faces = 0;
   chunk->buffer = 0;
   chunk->sign_buffer = 0;
   dirty_chunk(chunk);
   signs = &chunk->signs;
   sign_list_alloc(signs, 16);
   db_load_signs(signs, p, q);
   block_map = &chunk->map;
   light_map = &chunk->lights;
   dx = p * CHUNK_SIZE - 1;
   dy = 0;
   dz = q * CHUNK_SIZE - 1;
   map_alloc(block_map, dx, dy, dz, 0x7fff);
   map_alloc(light_map, dx, dy, dz, 0xf);
}

static void create_chunk(Chunk *chunk, int p, int q)
{
   WorkerItem _item;
   WorkerItem *item;

   init_chunk(chunk, p, q);

   item = &_item;
   item->p = chunk->p;
   item->q = chunk->q;
   item->block_maps[1][1] = &chunk->map;
   item->light_maps[1][1] = &chunk->lights;
   load_chunk(item);

   request_chunk(p, q);
}

static void delete_chunks(void)
{
   int i;
   Model *g = (Model*)&model;
   int count = g->chunk_count;
   State *s1 = &g->players->state;
   State *s2 = &(g->players + g->observe1)->state;
   State *s3 = &(g->players + g->observe2)->state;
   State *states[3] = {s1, s2, s3};
   for (i = 0; i < count; i++)
   {
      int j;
      Chunk *chunk = g->chunks + i;
      int delete = 1;

      for (j = 0; j < 3; j++)
      {
         State *s = states[j];
         int p = chunked(s->x);
         int q = chunked(s->z);
         if (chunk_distance(chunk, p, q) < g->delete_radius)
         {
            delete = 0;
            break;
         }
      }

      if (delete)
      {
         Chunk *other;

         map_free(&chunk->map);
         map_free(&chunk->lights);
         sign_list_free(&chunk->signs);
         renderer_del_buffer(chunk->buffer);
         renderer_del_buffer(chunk->sign_buffer);
         other = g->chunks + (--count);
         memcpy(chunk, other, sizeof(Chunk));
      }
   }
   g->chunk_count = count;
}

static void delete_all_chunks(void)
{
   int i;
   Model *g = (Model*)&model;
   for (i = 0; i < g->chunk_count; i++)
   {
      Chunk *chunk = g->chunks + i;
      map_free(&chunk->map);
      map_free(&chunk->lights);
      sign_list_free(&chunk->signs);
      renderer_del_buffer(chunk->buffer);
      renderer_del_buffer(chunk->sign_buffer);
   }
   g->chunk_count = 0;
}

static void check_workers(void)
{
   int i;
   for (i = 0; i < WORKERS; i++)
   {
      Model *g = (Model*)&model;
      Worker *worker = g->workers + i;
      mtx_lock(&worker->mtx);
      if (worker->state == WORKER_DONE)
      {
         int a;
         WorkerItem *item = &worker->item;
         Chunk *chunk = find_chunk(item->p, item->q);
         if (chunk)
         {
            if (item->load)
            {
               Map *block_map = item->block_maps[1][1];
               Map *light_map = item->light_maps[1][1];
               map_free(&chunk->map);
               map_free(&chunk->lights);
               map_copy(&chunk->map, block_map);
               map_copy(&chunk->lights, light_map);
               request_chunk(item->p, item->q);
            }
            generate_chunk(chunk, item);
         }
         for (a = 0; a < 3; a++)
         {
            int b;
            for (b = 0; b < 3; b++)
            {
               Map *block_map = item->block_maps[a][b];
               Map *light_map = item->light_maps[a][b];
               if (block_map)
               {
                  map_free(block_map);
                  free(block_map);
               }

               if (light_map)
               {
                  map_free(light_map);
                  free(light_map);
               }
            }
         }
         worker->state = WORKER_IDLE;
      }
      mtx_unlock(&worker->mtx);
   }
}

static void force_chunks(Player *player)
{
   int dp;
   State *s = &player->state;
   int p = chunked(s->x);
   int q = chunked(s->z);
   int r = 1;
   for (dp = -r; dp <= r; dp++)
   {
      int dq;
      for (dq = -r; dq <= r; dq++)
      {
         int a = p + dp;
         int b = q + dq;
         Chunk *chunk = find_chunk(a, b);
         Model *g = (Model*)&model;
         if (chunk)
         {
            if (chunk->dirty)
               gen_chunk_buffer(chunk);
         }
         else if (g->chunk_count < MAX_CHUNKS)
         {
            chunk = g->chunks + g->chunk_count++;
            create_chunk(chunk, a, b);
            gen_chunk_buffer(chunk);
         }
      }
   }
}

static void ensure_chunks_worker(Player *player, Worker *worker)
{
   int best_score, best_a, best_b;
   int start = 0x0fffffff;
   State *s = &player->state;
   float matrix[16];
   Model *g = (Model*)&model;
   set_matrix_3d(
         matrix, g->width, g->height,
         s->x, s->y, s->z, s->rx, s->ry, g->fov, g->ortho, RENDER_CHUNK_RADIUS);

   {
      int dp;
      float planes[6][4];
      int p = chunked(s->x);
      int q = chunked(s->z);
      int r = g->create_radius;
      best_score = start;
      best_a = 0;
      best_b = 0;
      frustum_planes(planes, RENDER_CHUNK_RADIUS, matrix);

      for (dp = -r; dp <= r; dp++)
      {
         int dq;
         for (dq = -r; dq <= r; dq++)
         {
            int score;
            int distance, invisible;
            Chunk *chunk;
            int priority = 0;
            int a = p + dp;
            int b = q + dq;
            int index = (ABS(a) ^ ABS(b)) % WORKERS;

            if (index != worker->index)
               continue;
            chunk = find_chunk(a, b);
            if (chunk && !chunk->dirty)
               continue;
            distance = MAX(ABS(dp), ABS(dq));
            invisible = !chunk_visible(planes, a, b, 0, MAX_BLOCK_HEIGHT);
            if (chunk)
               priority = chunk->buffer && chunk->dirty;
            score = (invisible << 24) | (priority << 16) | distance;
            if (score < best_score)
            {
               best_score = score;
               best_a = a;
               best_b = b;
            }
         }
      }
   }

   if (best_score == start)
      return;

   {
      int dp;
      int a = best_a;
      int b = best_b;
      int load = 0;
      Chunk *chunk = find_chunk(a, b);
      if (!chunk)
      {
         load = 1;
         if (g->chunk_count < MAX_CHUNKS)
         {
            chunk = g->chunks + g->chunk_count++;
            init_chunk(chunk, a, b);
         }
         else
            return;
      }

      {
         WorkerItem *item = &worker->item;
         item->p = chunk->p;
         item->q = chunk->q;
         item->load = load;
         for (dp = -1; dp <= 1; dp++)
         {
            int dq;
            for (dq = -1; dq <= 1; dq++)
            {
               Chunk *other = chunk;
               if (dp || dq)
                  other = find_chunk(chunk->p + dp, chunk->q + dq);

               if (other)
               {
                  Map *light_map;
                  Map *block_map = malloc(sizeof(Map));
                  map_copy(block_map, &other->map);
                  light_map = malloc(sizeof(Map));
                  map_copy(light_map, &other->lights);
                  item->block_maps[dp + 1][dq + 1] = block_map;
                  item->light_maps[dp + 1][dq + 1] = light_map;
               }
               else
               {
                  item->block_maps[dp + 1][dq + 1] = 0;
                  item->light_maps[dp + 1][dq + 1] = 0;
               }
            }
         }
         chunk->dirty = 0;
         worker->state = WORKER_BUSY;
         cnd_signal(&worker->cnd);
      }
   }
}

static void ensure_chunks(Player *player)
{
   int i;
   check_workers();
   force_chunks(player);

   for (i = 0; i < WORKERS; i++)
   {
      Model *g = (Model*)&model;
      Worker *worker = g->workers + i;
      mtx_lock(&worker->mtx);
      if (worker->state == WORKER_IDLE)
         ensure_chunks_worker(player, worker);
      mtx_unlock(&worker->mtx);
   }
}

static int worker_run(void *arg)
{
    Worker *worker = (Worker *)arg;
    int running = 1;
    while (running)
    {
       WorkerItem *item;

       mtx_lock(&worker->mtx);
       while (worker->state != WORKER_BUSY)
          cnd_wait(&worker->cnd, &worker->mtx);
       mtx_unlock(&worker->mtx);
       item = &worker->item;
       if (item->load)
          load_chunk(item);
       compute_chunk(item);
       mtx_lock(&worker->mtx);
       worker->state = WORKER_DONE;
       mtx_unlock(&worker->mtx);
    }
    return 0;
}

static void unset_sign(int x, int y, int z)
{
    int p = chunked(x);
    int q = chunked(z);
    Chunk *chunk = find_chunk(p, q);
    if (chunk)
    {
        SignList *signs = &chunk->signs;
        if (sign_list_remove_all(signs, x, y, z))
        {
            chunk->dirty = 1;
            db_delete_signs(x, y, z);
        }
    }
    else
        db_delete_signs(x, y, z);
}

static void unset_sign_face(int x, int y, int z, int face)
{
    int p = chunked(x);
    int q = chunked(z);
    Chunk *chunk = find_chunk(p, q);
    if (chunk)
    {
        SignList *signs = &chunk->signs;
        if (sign_list_remove(signs, x, y, z, face))
        {
            chunk->dirty = 1;
            db_delete_sign(x, y, z, face);
        }
    }
    else
        db_delete_sign(x, y, z, face);
}

static void _set_sign(
    int p, int q, int x, int y, int z, int face, const char *text, int dirty)
{
   Chunk *chunk;
   if (strlen(text) == 0)
   {
      unset_sign_face(x, y, z, face);
      return;
   }
   chunk = find_chunk(p, q);

   if (chunk)
   {
      SignList *signs = &chunk->signs;
      sign_list_add(signs, x, y, z, face, text);
      if (dirty)
         chunk->dirty = 1;
   }

   db_insert_sign(p, q, x, y, z, face, text);
}

static void set_sign(int x, int y, int z, int face, const char *text)
{
    int p = chunked(x);
    int q = chunked(z);
    _set_sign(p, q, x, y, z, face, text, 1);
    client_sign(x, y, z, face, text);
}

static void toggle_light(int x, int y, int z)
{
    int p = chunked(x);
    int q = chunked(z);
    Chunk *chunk = find_chunk(p, q);
    if (chunk) {
        Map *map = &chunk->lights;
        int w = map_get(map, x, y, z) ? 0 : 15;
        map_set(map, x, y, z, w);
        db_insert_light(p, q, x, y, z, w);
        client_light(x, y, z, w);
        dirty_chunk(chunk);
    }
}

static void set_light(int p, int q, int x, int y, int z, int w)
{
   Chunk *chunk = find_chunk(p, q);
   if (chunk)
   {
      Map *map = &chunk->lights;
      if (map_set(map, x, y, z, w))
      {
         dirty_chunk(chunk);
         db_insert_light(p, q, x, y, z, w);
      }
   }
   else
      db_insert_light(p, q, x, y, z, w);
}

static void _set_block(int p, int q, int x, int y, int z, int w, int dirty)
{
    Chunk *chunk = find_chunk(p, q);
    if (chunk)
    {
        Map *map = &chunk->map;
        if (map_set(map, x, y, z, w))
        {
            if (dirty)
                dirty_chunk(chunk);
            db_insert_block(p, q, x, y, z, w);
        }
    }
    else
        db_insert_block(p, q, x, y, z, w);

    if (w == 0 && chunked(x) == p && chunked(z) == q)
    {
        unset_sign(x, y, z);
        set_light(p, q, x, y, z, 0);
    }
}

static void set_block(int x, int y, int z, int w)
{
   int dx;
   int p = chunked(x);
   int q = chunked(z);

   _set_block(p, q, x, y, z, w, 1);

   for (dx = -1; dx <= 1; dx++)
   {
      int dz;
      for (dz = -1; dz <= 1; dz++)
      {
         if (dx == 0 && dz == 0)
            continue;
         if (dx && chunked(x + dx) == p)
            continue;
         if (dz && chunked(z + dz) == q)
            continue;
         _set_block(p + dx, q + dz, x, y, z, -w, 1);
      }
   }
   client_block(x, y, z, w);
}

static void record_block(int x, int y, int z, int w)
{
   Model *g = (Model*)&model;
   memcpy(&g->block1, &g->block0, sizeof(Block));
   g->block0.x = x;
   g->block0.y = y;
   g->block0.z = z;
   g->block0.w = w;
}

static int get_block(int x, int y, int z)
{
    int p        = chunked(x);
    int q        = chunked(z);
    Chunk *chunk = find_chunk(p, q);

    if (chunk)
    {
        Map *map = &chunk->map;
        return map_get(map, x, y, z);
    }
    return 0;
}

static void builder_block(int x, int y, int z, int w)
{
   if (y <= 0 || y >= MAX_BLOCK_HEIGHT)
      return;
   if (is_destructable(get_block(x, y, z)))
      set_block(x, y, z, 0);
   if (w)
      set_block(x, y, z, w);
}

static int render_chunks(Attrib *attrib, Player *player)
{
   unsigned i;
   float matrix[16];
   float planes[6][4];
   struct shader_program_info info = {0};
   int result                      = 0;
   State *s                        = &player->state;
   ensure_chunks(player);

   {
      int p                           = chunked(s->x);
      int q                           = chunked(s->z);
      float light                     = get_daylight();
      Model *g = (Model*)&model;

      set_matrix_3d(
            matrix, g->width, g->height,
            s->x, s->y, s->z, s->rx, s->ry, g->fov, g->ortho, RENDER_CHUNK_RADIUS);
      frustum_planes(planes, RENDER_CHUNK_RADIUS, matrix);

      info.attrib          = attrib;
      info.program.enable  = true;
      info.matrix.enable   = true;
      info.matrix.data     = &matrix[0];
      info.sampler.enable  = true;
      info.camera.enable   = true;
      info.camera.x        = s->x;
      info.camera.y        = s->y;
      info.camera.z        = s->z;
      info.sampler.data    = 0;
      info.extra1.enable   = true;
      info.extra1.data     = 2;
      info.extra2.enable   = true;
      info.extra2.data     = light;
      info.extra3.enable   = true;
      info.extra3.data     = RENDER_CHUNK_RADIUS * CHUNK_SIZE;
      info.extra4.enable   = true;
      info.extra4.data     = g->ortho;
      info.timer.enable    = true;
      info.timer.data      = time_of_day();

      render_shader_program(&info);

      for (i = 0; i < g->chunk_count; i++)
      {
         Chunk *chunk = g->chunks + i;

         if (chunk_distance(chunk, p, q) > RENDER_CHUNK_RADIUS)
            continue;

         if (!chunk_visible(
                  planes, chunk->p, chunk->q, chunk->miny, chunk->maxy))
            continue;

         draw_triangles_3d_ao(attrib, chunk->buffer, chunk->faces * 6);
         result += chunk->faces;
      }
   }
   return result;
}

static void render_water(Attrib *attrib, Player *player)
{
   struct shader_program_info info = {0};
   uintptr_t buffer;
   float matrix[16];
   State *s = &player->state;
   float light = get_daylight();
   Model *g = (Model*)&model;

   set_matrix_3d(
         matrix, g->width, g->height,
         s->x, s->y, s->z, s->rx, s->ry, g->fov, g->ortho,
         RENDER_CHUNK_RADIUS);

   info.attrib          = attrib;
   info.program.enable  = true;
   info.matrix.enable   = true;
   info.matrix.data     = &matrix[0];
   info.camera.enable   = true;
   info.camera.x        = s->x;
   info.camera.y        = s->y;
   info.camera.z        = s->z;
   info.extra1.enable   = true;
   info.extra1.data     = 2;
   info.extra2.enable   = true;
   info.extra2.data     = light;
   info.extra3.enable   = true;
   info.extra3.data     = RENDER_CHUNK_RADIUS * CHUNK_SIZE;
   info.extra4.enable   = true;
   info.extra4.data     = g->ortho;
   info.timer.enable    = true;
   info.timer.data      = time_of_day();

   render_shader_program(&info);

   renderer_enable_blend();

   buffer = gen_water_buffer(
         s->x, 11 + sinf(glfwGetTime() * 2) * 0.05, s->z,
         RENDER_CHUNK_RADIUS * CHUNK_SIZE);
   draw_triangles_3d_ao(attrib, buffer, 12);
   renderer_del_buffer(buffer);
   renderer_disable_blend();
}

static void render_signs(Attrib *attrib, Player *player)
{
   unsigned i;
   struct shader_program_info info = {0};
   State *s = &player->state;
   int p = chunked(s->x);
   int q = chunked(s->z);
   float matrix[16];
   Model *g = (Model*)&model;

   set_matrix_3d(
         matrix, g->width, g->height,
         s->x, s->y, s->z, s->rx, s->ry, g->fov, g->ortho, RENDER_CHUNK_RADIUS);

   {
      float planes[6][4];
      frustum_planes(planes, RENDER_CHUNK_RADIUS, matrix);

      info.attrib          = attrib;
      info.program.enable  = true;
      info.matrix.enable   = true;
      info.matrix.data     = &matrix[0];
      info.sampler.enable  = true;
      info.sampler.data    = 3;
      info.extra1.enable   = true;
      info.extra1.data     = 1;

      render_shader_program(&info);

      for (i = 0; i < g->chunk_count; i++)
      {
         Chunk *chunk = g->chunks + i;
         if (chunk_distance(chunk, p, q) > g->sign_radius)
            continue;

         if (!chunk_visible(
                  planes, chunk->p, chunk->q, chunk->miny, chunk->maxy))
            continue;

         /* draw signs */
         renderer_enable_polygon_offset_fill();
         draw_triangles_3d_text(attrib, chunk->sign_buffer, chunk->sign_faces * 6);
         renderer_disable_polygon_offset_fill();
      }
   }
}

static void render_sign(Attrib *attrib, Player *player)
{
   float matrix[16];
   int x, y, z, face;
   uintptr_t buffer;
   int length;
   State *s                        = NULL;
   float *data                     = NULL;
   char text[MAX_SIGN_LENGTH]      = {0};
   struct shader_program_info info = {0};
   Model *g = (Model*)&model;

   if (!g->typing || g->typing_buffer[0] != CRAFT_KEY_SIGN)
      return;
   if (!hit_test_face(player, &x, &y, &z, &face))
      return;

   s = &player->state;

   set_matrix_3d(
         matrix, g->width, g->height,
         s->x, s->y, s->z, s->rx, s->ry,
         g->fov, g->ortho, RENDER_CHUNK_RADIUS);

   info.attrib          = attrib;
   info.program.enable  = true;
   info.matrix.enable   = true;
   info.matrix.data     = &matrix[0];
   info.sampler.enable  = true;
   info.sampler.data    = 3;
   info.extra1.enable   = true;
   info.extra1.data     = 1;

   render_shader_program(&info);

   strncpy(text, g->typing_buffer + 1, MAX_SIGN_LENGTH);
   text[MAX_SIGN_LENGTH - 1] = '\0';

   data   = malloc_faces(5, strlen(text));
   length = _gen_sign_buffer(data, x, y, z, face, text);
   buffer = renderer_gen_faces(5, length, data);

   /* draw sign */
   renderer_enable_polygon_offset_fill();
   draw_triangles_3d_text(attrib, buffer, length * 6);
   renderer_disable_polygon_offset_fill();

   renderer_del_buffer(buffer);
}

static void render_players(Attrib *attrib, Player *player)
{
   unsigned i;
   float matrix[16];
   struct shader_program_info info = {0};
   State *s = &player->state;
   Model *g = (Model*)&model;

   set_matrix_3d(
         matrix, g->width, g->height,
         s->x, s->y, s->z, s->rx, s->ry, g->fov, g->ortho, RENDER_CHUNK_RADIUS);

   info.attrib          = attrib;
   info.program.enable  = true;
   info.matrix.enable   = true;
   info.matrix.data     = &matrix[0];
   info.camera.enable   = true;
   info.camera.x        = s->x;
   info.camera.y        = s->y;
   info.camera.z        = s->z;
   info.sampler.enable  = true;
   info.sampler.data    = 0;
   info.timer.enable    = true;
   info.timer.data      = time_of_day();

   render_shader_program(&info);

   for (i = 0; i < g->player_count; i++)
   {
      Player *other = g->players + i;

      /* draw player? */
      if (other != player)
         draw_triangles_3d_ao(attrib, other->buffer, 36);
   }
}

static void render_sky(Attrib *attrib, Player *player, uintptr_t buffer)
{
   float matrix[16];
   struct shader_program_info info = {0};
   State *s = &player->state;
   Model *g = (Model*)&model;

   set_matrix_3d(
         matrix, g->width, g->height,
         0, 0, 0, s->rx, s->ry, g->fov, 0, RENDER_CHUNK_RADIUS);

   info.attrib          = attrib;
   info.program.enable  = true;
   info.matrix.enable   = true;
   info.matrix.data     = &matrix[0];
   info.sampler.enable  = true;
   info.sampler.data    = 2;
   info.timer.enable    = true;
   info.timer.data      = time_of_day();

   render_shader_program(&info);

   draw_triangles_3d(attrib, buffer, 512 * 3);
}

static void render_wireframe(Attrib *attrib, Player *player)
{
   int hw, hx, hy, hz;
   uintptr_t wireframe_buffer;
   float matrix[16];
   struct shader_program_info info = {0};
   State *s = &player->state;
   Model *g = (Model*)&model;

   set_matrix_3d(
         matrix, g->width, g->height,
         s->x, s->y, s->z, s->rx, s->ry, g->fov, g->ortho, RENDER_CHUNK_RADIUS);

   hw = hit_test(0, s->x, s->y, s->z, s->rx, s->ry, &hx, &hy, &hz);
   if (!is_obstacle(hw))
      return;

   renderer_enable_color_logic_op();

   info.attrib           = attrib;
   info.program.enable   = true;
   info.linewidth.enable = true;
   info.linewidth.data   = 1;
   info.matrix.enable    = true;
   info.matrix.data      = &matrix[0];

   render_shader_program(&info);

   wireframe_buffer = gen_wireframe_buffer(hx, hy, hz, 0.53);
   draw_lines(attrib, wireframe_buffer, 3, 24);
   renderer_del_buffer(wireframe_buffer);
   renderer_disable_color_logic_op();
}

static void render_crosshairs(Attrib *attrib)
{
   float matrix[16];
   uintptr_t crosshair_buffer;
   struct shader_program_info info = {0};
   Model *g = (Model*)&model;

   set_matrix_2d(matrix, g->width, g->height);

   renderer_enable_color_logic_op();

   info.attrib           = attrib;
   info.program.enable   = true;
   info.linewidth.enable = true;
   info.linewidth.data   = 4 * g->scale;
   info.matrix.enable    = true;
   info.matrix.data      = &matrix[0];

   render_shader_program(&info);

   crosshair_buffer = gen_crosshair_buffer();

   draw_lines(attrib, crosshair_buffer, 2, 4);
   renderer_del_buffer(crosshair_buffer);
   renderer_disable_color_logic_op();
}

static void render_item(Attrib *attrib)
{
   int w;
   float matrix[16];
   uintptr_t buffer;
   unsigned count                  = 0;
   struct shader_program_info info = {0};
   Model *g = (Model*)&model;

   set_matrix_item(matrix, g->width, g->height, g->scale);

   info.attrib          = attrib;
   info.program.enable  = true;
   info.matrix.enable   = true;
   info.matrix.data     = &matrix[0];
   info.camera.enable   = true;
   info.camera.x        = 0.0;
   info.camera.y        = 0.0;
   info.camera.z        = 5.0;
   info.sampler.enable  = true;
   info.sampler.data    = 0;
   info.timer.enable    = true;
   info.timer.data      = time_of_day();

   render_shader_program(&info);

   w = items[g->item_index];
   if (is_plant(w))
   {
      buffer = gen_plant_buffer(0, 0, 0, 0.5, w);
      count  = 24;
   }
   else
   {
      buffer = gen_cube_buffer(0, 0, 0, 0.5, w);
      count  = 36;
   }

   draw_triangles_3d_ao(attrib, buffer, count);

   renderer_del_buffer(buffer);
}

static void render_text(
    Attrib *attrib, int justify, float x, float y, float n, char *text)
{
   int length;
   uintptr_t buffer;
   float matrix[16];
   struct shader_program_info info = {0};
   Model *g = (Model*)&model;

   set_matrix_2d(matrix, g->width, g->height);

   info.attrib          = attrib;
   info.program.enable  = true;
   info.matrix.enable   = true;
   info.matrix.data     = &matrix[0];
   info.sampler.enable  = true;
   info.sampler.data    = 1;
   info.extra1.enable   = true;
   info.extra1.data     = 0;

   render_shader_program(&info);

   length   = strlen(text);
   x       -= n * justify * (length - 1) / 2;
   buffer   = gen_text_buffer(x, y, n, text);

   /* draw text */
   renderer_enable_blend();
   draw_triangles_2d(attrib, buffer, length * 6);
   renderer_disable_blend();

   renderer_del_buffer(buffer);
}

static void add_message(const char *text)
{
   Model *g = (Model*)&model;

   printf("%s\n", text);
   snprintf(
         g->messages[g->message_index], MAX_TEXT_LENGTH, "%s", text);
   g->message_index = (g->message_index + 1) % MAX_MESSAGES;
}

static void login(void)
{
   char username[128]       = {0};
   char identity_token[128] = {0};
   char access_token[128]   = {0};

   if (db_auth_get_selected(username, 128, identity_token, 128))
   {
      printf("Contacting login server for username: %s\n", username);
      if (get_access_token(
               access_token, 128, username, identity_token))
      {
         printf("Successfully authenticated with the login server\n");
         client_login(username, access_token);
      }
      else
      {
         printf("Failed to authenticate with the login server\n");
         client_login("", "");
      }
   }
   else
   {
      printf("Logging in anonymously\n");
      client_login("", "");
   }
}

static void paste(void)
{
   int x, y, z;
   Model *g = (Model*)&model;
   Block *c1 = &g->copy1;
   Block *c2 = &g->copy0;
   Block *p1 = &g->block1;
   Block *p2 = &g->block0;
   int scx   = SIGN(c2->x - c1->x);
   int scz   = SIGN(c2->z - c1->z);
   int spx   = SIGN(p2->x - p1->x);
   int spz   = SIGN(p2->z - p1->z);
   int oy    = p1->y - c1->y;
   int dx    = ABS(c2->x - c1->x);
   int dz    = ABS(c2->z - c1->z);

   for (y = 0; y < MAX_BLOCK_HEIGHT; y++)
   {
      for (x = 0; x <= dx; x++)
      {
         for (z = 0; z <= dz; z++)
         {
            int w = get_block(c1->x + x * scx, y, c1->z + z * scz);
            builder_block(p1->x + x * spx, y + oy, p1->z + z * spz, w);
         }
      }
   }
}

static void array(Block *b1, Block *b2, int xc, int yc, int zc)
{
   int i, j, k;
   int w, dx, dy, dz;
   if (b1->w != b2->w)
      return;

   w  = b1->w;
   dx = b2->x - b1->x;
   dy = b2->y - b1->y;
   dz = b2->z - b1->z;
   xc = dx ? xc : 1;
   yc = dy ? yc : 1;
   zc = dz ? zc : 1;

   for (i = 0; i < xc; i++)
   {
      int x = b1->x + dx * i;
      for (j = 0; j < yc; j++)
      {
         int y = b1->y + dy * j;
         for (k = 0; k < zc; k++)
         {
            int z = b1->z + dz * k;
            builder_block(x, y, z, w);
         }
      }
   }
}

static void cube(Block *b1, Block *b2, int fill)
{
   int x, y, z, w;
   int x1, y1, z1, x2, y2, z2, a;

   if (b1->w != b2->w)
      return;

   w  = b1->w;
   x1 = MIN(b1->x, b2->x);
   y1 = MIN(b1->y, b2->y);
   z1 = MIN(b1->z, b2->z);
   x2 = MAX(b1->x, b2->x);
   y2 = MAX(b1->y, b2->y);
   z2 = MAX(b1->z, b2->z);
   a = (x1 == x2) + (y1 == y2) + (z1 == z2);

   for (x = x1; x <= x2; x++)
   {
      for (y = y1; y <= y2; y++)
      {
         for (z = z1; z <= z2; z++)
         {
            if (!fill)
            {
               int n = 0;
               n += x == x1 || x == x2;
               n += y == y1 || y == y2;
               n += z == z1 || z == z2;
               if (n <= a)
                  continue;
            }
            builder_block(x, y, z, w);
         }
      }
   }
}

static void sphere(Block *center, int radius, int fill, int fx, int fy, int fz)
{
    static const float offsets[8][3] = {
        {-0.5, -0.5, -0.5},
        {-0.5, -0.5, 0.5},
        {-0.5, 0.5, -0.5},
        {-0.5, 0.5, 0.5},
        {0.5, -0.5, -0.5},
        {0.5, -0.5, 0.5},
        {0.5, 0.5, -0.5},
        {0.5, 0.5, 0.5}
    };
    int x;
    int cx = center->x;
    int cy = center->y;
    int cz = center->z;
    int w = center->w;

    for (x = cx - radius; x <= cx + radius; x++)
    {
       int y;
        if (fx && x != cx)
            continue;

        for (y = cy - radius; y <= cy + radius; y++)
        {
           int z;

            if (fy && y != cy)
                continue;
            for (z = cz - radius; z <= cz + radius; z++)
            {
               int i, inside = 0, outside;
                if (fz && z != cz)
                    continue;

                outside = fill;

                for (i = 0; i < 8; i++)
                {
                   float dx = x + offsets[i][0] - cx;
                   float dy = y + offsets[i][1] - cy;
                   float dz = z + offsets[i][2] - cz;
                   float d  = sqrtf(dx * dx + dy * dy + dz * dz);
                   if (d < radius)
                      inside = 1;
                   else
                      outside = 1;
                }

                if (inside && outside)
                    builder_block(x, y, z, w);
            }
        }
    }
}

static void cylinder(Block *b1, Block *b2, int radius, int fill)
{
    if (b1->w != b2->w)
        return;
    {
       int w = b1->w;
       int x1 = MIN(b1->x, b2->x);
       int y1 = MIN(b1->y, b2->y);
       int z1 = MIN(b1->z, b2->z);
       int x2 = MAX(b1->x, b2->x);
       int y2 = MAX(b1->y, b2->y);
       int z2 = MAX(b1->z, b2->z);
       int fx = x1 != x2;
       int fy = y1 != y2;
       int fz = z1 != z2;
       if (fx + fy + fz != 1)
          return;
       {
          Block block = {x1, y1, z1, w};
          if (fx)
          {
             int x;
             for ( x = x1; x <= x2; x++)
             {
                block.x = x;
                sphere(&block, radius, fill, 1, 0, 0);
             }
          }
          if (fy)
          {
             int y;
             for (y = y1; y <= y2; y++)
             {
                block.y = y;
                sphere(&block, radius, fill, 0, 1, 0);
             }
          }
          if (fz)
          {
             int z;
             for (z = z1; z <= z2; z++)
             {
                block.z = z;
                sphere(&block, radius, fill, 0, 0, 1);
             }
          }
       }
    }
}

static void tree(Block *block)
{
   int dx, dz, y;
   int bx = block->x;
   int by = block->y;
   int bz = block->z;

   for (y = by + 3; y < by + 8; y++)
   {
      for (dx = -3; dx <= 3; dx++)
      {
         for (dz = -3; dz <= 3; dz++)
         {
            int dy = y - (by + 4);
            int d = (dx * dx) + (dy * dy) + (dz * dz);
            if (d < 11)
               builder_block(bx + dx, y, bz + dz, 15);
         }
      }
   }

   for (y = by; y < by + 7; y++)
      builder_block(bx, y, bz, 5);
}

static void main_set_db_path(void)
{
   const char *dir = NULL;
   Model *g = (Model*)&model;

   if (environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &dir) && dir && *dir)
   {
#ifdef _WIN32
      char slash = '\\';
#else
      char slash = '/';
#endif
      snprintf(g->db_path, MAX_PATH_LENGTH, "%s%c%s", dir, slash, DB_PATH);
   }
   else
      snprintf(g->db_path, MAX_PATH_LENGTH, "%s", DB_PATH);
}

static void parse_command(const char *buffer, int forward)
{
    char username[128] = {0};
    char token[128] = {0};
    char server_addr[MAX_ADDR_LENGTH];
    int server_port = DEFAULT_PORT;
    char filename[MAX_PATH_LENGTH];
    int radius, count, xc, yc, zc;
    Model *g = (Model*)&model;

    if (sscanf(buffer, "/identity %128s %128s", username, token) == 2) {
        db_auth_set(username, token);
        add_message("Successfully imported identity token!");
        login();
    }
    else if (strcmp(buffer, "/logout") == 0) {
        db_auth_select_none();
        login();
    }
    else if (sscanf(buffer, "/login %128s", username) == 1)
    {
        if (db_auth_select(username))
            login();
        else
            add_message("Unknown username.");
    }
    else if (sscanf(buffer,
        "/online %128s %d", server_addr, &server_port) >= 1)
    {
        g->mode_changed = 1;
        g->mode = MODE_ONLINE;
        strncpy(g->server_addr, server_addr, MAX_ADDR_LENGTH);
        g->server_port = server_port;
        snprintf(g->db_path, MAX_PATH_LENGTH,
            "cache.%s.%d.db", g->server_addr, g->server_port);
    }
    else if (sscanf(buffer, "/offline %128s", filename) == 1) {
        g->mode_changed = 1;
        g->mode = MODE_OFFLINE;
        snprintf(g->db_path, MAX_PATH_LENGTH, "%s.db", filename);
    }
    else if (strcmp(buffer, "/offline") == 0) {
        g->mode_changed = 1;
        g->mode = MODE_OFFLINE;
        main_set_db_path();
    }
    else if (sscanf(buffer, "/view %d", &radius) == 1) {
        if (radius >= 1 && radius <= 24) {
            g->create_radius = radius;
            g->delete_radius = radius + 4;
        }
        else {
            add_message("Viewing distance must be between 1 and 24.");
        }
    }
    else if (strcmp(buffer, "/copy") == 0)
    {
       memcpy(&g->copy0, &g->block0, sizeof(Block));
       memcpy(&g->copy1, &g->block1, sizeof(Block));
    }
    else if (strcmp(buffer, "/paste") == 0)
    {
        paste();
    }
    else if (strcmp(buffer, "/tree") == 0)
    {
        tree(&g->block0);
    }
    else if (sscanf(buffer, "/array %d %d %d", &xc, &yc, &zc) == 3) {
        array(&g->block1, &g->block0, xc, yc, zc);
    }
    else if (sscanf(buffer, "/array %d", &count) == 1) {
        array(&g->block1, &g->block0, count, count, count);
    }
    else if (strcmp(buffer, "/fcube") == 0) {
        cube(&g->block0, &g->block1, 1);
    }
    else if (strcmp(buffer, "/cube") == 0) {
        cube(&g->block0, &g->block1, 0);
    }
    else if (sscanf(buffer, "/fsphere %d", &radius) == 1) {
        sphere(&g->block0, radius, 1, 0, 0, 0);
    }
    else if (sscanf(buffer, "/sphere %d", &radius) == 1) {
        sphere(&g->block0, radius, 0, 0, 0, 0);
    }
    else if (sscanf(buffer, "/fcirclex %d", &radius) == 1) {
        sphere(&g->block0, radius, 1, 1, 0, 0);
    }
    else if (sscanf(buffer, "/circlex %d", &radius) == 1) {
        sphere(&g->block0, radius, 0, 1, 0, 0);
    }
    else if (sscanf(buffer, "/fcircley %d", &radius) == 1) {
        sphere(&g->block0, radius, 1, 0, 1, 0);
    }
    else if (sscanf(buffer, "/circley %d", &radius) == 1) {
        sphere(&g->block0, radius, 0, 0, 1, 0);
    }
    else if (sscanf(buffer, "/fcirclez %d", &radius) == 1) {
        sphere(&g->block0, radius, 1, 0, 0, 1);
    }
    else if (sscanf(buffer, "/circlez %d", &radius) == 1) {
        sphere(&g->block0, radius, 0, 0, 0, 1);
    }
    else if (sscanf(buffer, "/fcylinder %d", &radius) == 1) {
        cylinder(&g->block0, &g->block1, radius, 1);
    }
    else if (sscanf(buffer, "/cylinder %d", &radius) == 1) {
        cylinder(&g->block0, &g->block1, radius, 0);
    }
    else if (forward) {
        client_talk(buffer);
    }
}

void on_light() {
   Model *g = (Model*)&model;
    State *s = &g->players->state;
    int hx, hy, hz;
    int hw = hit_test(0, s->x, s->y, s->z, s->rx, s->ry, &hx, &hy, &hz);
    if (hy > 0 && hy < MAX_BLOCK_HEIGHT && is_destructable(hw)) {
        toggle_light(hx, hy, hz);
    }
}

void on_left_click(void)
{
   int hx, hy, hz;
   Model *g = (Model*)&model;
   State *s        = &g->players->state;
   int hw          = hit_test(0, s->x, s->y, s->z, s->rx, s->ry, &hx, &hy, &hz);

   if (hy > 0 && hy < MAX_BLOCK_HEIGHT && is_destructable(hw))
   {
      set_block(hx, hy, hz, 0);
      record_block(hx, hy, hz, 0);
      if (is_plant(get_block(hx, hy + 1, hz)))
         set_block(hx, hy + 1, hz, 0);
   }
}

void on_right_click(void)
{
   Model *g = (Model*)&model;
    State *s = &g->players->state;
    int hx, hy, hz;
    int hw = hit_test(1, s->x, s->y, s->z, s->rx, s->ry, &hx, &hy, &hz);
    if (hy > 0 && hy < MAX_BLOCK_HEIGHT && is_obstacle(hw)) {
        if (!player_intersects_block(2, s->x, s->y, s->z, hx, hy, hz)) {
            set_block(hx, hy, hz, items[g->item_index]);
            record_block(hx, hy, hz, items[g->item_index]);
        }
    }
}

void on_middle_click(void)
{
   int i;
   Model *g = (Model*)&model;
   State *s = &g->players->state;
   int hx, hy, hz;
   int hw = hit_test(0, s->x, s->y, s->z, s->rx, s->ry, &hx, &hy, &hz);

   for (i = 0; i < item_count; i++)
   {
      if (items[i] == hw)
      {
         g->item_index = i;
         break;
      }
   }
}

void on_key(void)
{
   Model *g = (Model*)&model;

   if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A))
   {
      if ((g->item_index + 1) < item_count)
         g->item_index++;
      else
         g->item_index = 0;
   }

   if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X))
   {
      if (g->typing)
      {
#if 0
         if (mods & GLFW_MOD_SHIFT) {
            int n = strlen(g->typing_buffer);
            if (n < MAX_TEXT_LENGTH - 1) {
               g->typing_buffer[n] = '\r';
               g->typing_buffer[n + 1] = '\0';
            }
         }
         else
#endif
         {
            g->typing = 0;
            if (g->typing_buffer[0] == CRAFT_KEY_SIGN) {
               Player *player = g->players;
               int x, y, z, face;
               if (hit_test_face(player, &x, &y, &z, &face)) {
                  set_sign(x, y, z, face, g->typing_buffer + 1);
               }
            }
            else if (g->typing_buffer[0] == '/') {
               parse_command(g->typing_buffer, 1);
            }
            else {
               client_talk(g->typing_buffer);
            }
         }
      }
      else
         on_right_click();
   }

   if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y))
   {
      if (g->typing)
      {
#if 0
         if (mods & GLFW_MOD_SHIFT) {
            int n = strlen(g->typing_buffer);
            if (n < MAX_TEXT_LENGTH - 1) {
               g->typing_buffer[n] = '\r';
               g->typing_buffer[n + 1] = '\0';
            }
         }
         else
#endif
         {
            g->typing = 0;
            if (g->typing_buffer[0] == CRAFT_KEY_SIGN) {
               Player *player = g->players;
               int x, y, z, face;
               if (hit_test_face(player, &x, &y, &z, &face)) {
                  set_sign(x, y, z, face, g->typing_buffer + 1);
               }
            }
            else if (g->typing_buffer[0] == '/') {
               parse_command(g->typing_buffer, 1);
            }
            else {
               client_talk(g->typing_buffer);
            }
         }
      }
      else
         on_left_click();
   }
}

void on_scroll(double xdelta, double ydelta)
{
    static double ypos = 0;
    Model *g = (Model*)&model;

    ypos += ydelta;

    if (ypos <= -SCROLL_THRESHOLD)
    {
        g->item_index = (g->item_index + 1) % item_count;
        ypos = 0;
    }
    if (ypos >= SCROLL_THRESHOLD)
    {
        g->item_index--;
        if (g->item_index < 0)
            g->item_index = item_count - 1;
        ypos = 0;
    }
}

static void handle_mouse_input(void)
{
    static int pmr = 0;
    static int pml = 0;
    static int pmm = 0;
    int mr, ml, mm, wu, wd;
    int16_t mx = input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_X);
    int16_t my = input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_Y);

    if (mx || my)
    {
       Model *g = (Model*)&model;
        State *s = &g->players->state;
        float m = 0.0025; // mouse sensitivity

        if (INVERTED_AIM)
            my = -my;

        s->rx += mx * m;
        s->ry += my * m;

        if (s->rx < 0)
            s->rx += RADIANS(360);

        if (s->rx >= RADIANS(360))
            s->rx -= RADIANS(360);

        s->ry = fmaxf_internal(s->ry, -RADIANS(90));
        s->ry = fminf_internal(s->ry, RADIANS(90));
    }

    mr = input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_RIGHT);
    ml = input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_LEFT);
    mm = input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_MIDDLE);
    wu = input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_WHEELUP);
    wd = input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_WHEELDOWN);

    if (pmr == 0 && mr == 1)  // Button press event
        on_right_click();

    if (pml == 0 && ml == 1)
        on_left_click();

    if (pmm == 0 && mm == 1)
        on_middle_click();

    if (wu == 1 || wd == 1)
        on_scroll(0, (wu - wd) * SCROLL_THRESHOLD);

    pmr = mr;
    pml = ml;
    pmm = mm;
}

void handle_movement(double dt)
{
   static float dy = 0;
   float sz = 0.0;
   float sx = 0.0;
   Model *g = (Model*)&model;
   State *s = &g->players->state;

   /* TODO/FIXME: hardcode this for now */
   if (JUMPING_FLASH_MODE)
      dt = 0.02;
   else
      dt = 0.0166;

   if (!g->typing)
   {
      float left_stick_x, left_stick_y;
      float right_stick_x, right_stick_y;
      float m = dt * 1.0;

      g->ortho = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT) ? 64 : 0;
      g->fov = FIELD_OF_VIEW; /* TODO: set to 15 for zoom */

      // JOYPAD INPUT //
      if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP))
         sz--;
      if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN))
         sz++;
      if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT))
         s->rx -= m;
      if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT))
         s->rx += m;
      if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L))
         sx--;
      if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R))
         sx++;
      if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2))
         s->ry += m;
      if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2))
         s->ry -= m;

      // ANALOG INPUT //
      // Get analog values
      right_stick_y = input_state_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y);
      right_stick_x = input_state_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X);
      left_stick_y  = input_state_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y);
      left_stick_x  = input_state_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X);

      if (right_stick_x || right_stick_y || left_stick_x || left_stick_y)
      {
         // Rescale to [-1, 1]
         const int16_t analog_min = -0x8000; // see libretro.h:136
         const int16_t analog_max = 0x7fff;
         right_stick_y = (((right_stick_y - analog_min) * 2.0) / (float)(analog_max - analog_min)) - 1.0;
         right_stick_x = (((right_stick_x - analog_min) * 2.0) / (float)(analog_max - analog_min)) - 1.0;
         left_stick_y  = (((left_stick_y  - analog_min) * 2.0) / (float)(analog_max - analog_min)) - 1.0;
         left_stick_x  = (((left_stick_x  - analog_min) * 2.0) / (float)(analog_max - analog_min)) - 1.0;

         // Invert aim
         if (INVERTED_AIM)
            right_stick_y = -right_stick_y;

         // Check deadzone and change state
         if (left_stick_y * left_stick_y + left_stick_x * left_stick_x > DEADZONE_RADIUS * DEADZONE_RADIUS)
         {
            sz += left_stick_y;
            sx += left_stick_x;
         }
         if (right_stick_y * right_stick_y + right_stick_x * right_stick_x > DEADZONE_RADIUS * DEADZONE_RADIUS)
         {
            s->rx += right_stick_x * ANALOG_SENSITIVITY;
            s->ry += right_stick_y * ANALOG_SENSITIVITY;
         }
      }

      // Keep x-rotation between [0, 360] degrees
      if (s->rx < 0)
         s->rx += RADIANS(360);
      if (s->rx >= RADIANS(360))
         s->rx -= RADIANS(360);

      // Keep y-rotation between [-90, 90] degrees
      s->ry = fminf_internal(s->ry, RADIANS(90));
      s->ry = fmaxf_internal(s->ry, -RADIANS(90));
   }

   {
      float vx, vy, vz;
      get_motion_vector(g->flying, sz, sx, s->rx, s->ry, &vx, &vy, &vz);
      if (!g->typing)
      {
         if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B))
         {
            if (g->flying)
               vy = 1;
            else if (dy == 0)
            {
               if (JUMPING_FLASH_MODE)
               {
                  s->ry = RADIANS(-90);
                  dy = 16;
               }
               else
                  dy = 8;
            }
         }
      }
      {
         int i;
         float speed = g->flying ? 20 : 5;
         int estimate = roundf(sqrtf(
                  powf(vx * speed, 2) +
                  powf(vy * speed + ABS(dy) * 2, 2) +
                  powf(vz * speed, 2)) * dt * 8);
         int step = MAX(8, estimate);
         float ut = dt / step;
         vx = vx * ut * speed;
         vy = vy * ut * speed;
         vz = vz * ut * speed;
         for (i = 0; i < step; i++)
         {
            if (g->flying)
               dy = 0;
            else
            {
               dy -= ut * 25;
               dy = MAX(dy, -250);
            }
            s->x += vx;
            s->y += vy + dy * ut;
            s->z += vz;
            if (collide(2, &s->x, &s->y, &s->z))
               dy = 0;
         }

         if (s->y < 0)
            s->y = highest_block(s->x, s->z) + 2;
      }
   }
}

static void parse_buffer(char *buffer)
{
   Model *g = (Model*)&model;
    Player *me = g->players;
    State *s = &g->players->state;
    char *key;
    char *line = tokenize(buffer, "\n", &key);
    while (line)
    {
        int pid;
        float ux, uy, uz, urx, ury;
        float px, py, pz, prx, pry;
        int bp, bq, bx, by, bz, bw;
        int face;
        char text[MAX_SIGN_LENGTH] = {0};
        char name[MAX_NAME_LENGTH];
        int kp, kq, kk;
        double elapsed;
        int day_length;
        char format[64];

        if (sscanf(line, "U,%d,%f,%f,%f,%f,%f",
            &pid, &ux, &uy, &uz, &urx, &ury) == 6)
        {
            me->id = pid;
            s->x = ux; s->y = uy; s->z = uz; s->rx = urx; s->ry = ury;
            force_chunks(me);
            if (uy == 0)
                s->y = highest_block(s->x, s->z) + 2;
        }
        if (sscanf(line, "B,%d,%d,%d,%d,%d,%d",
            &bp, &bq, &bx, &by, &bz, &bw) == 6)
        {
            _set_block(bp, bq, bx, by, bz, bw, 0);
            if (player_intersects_block(2, s->x, s->y, s->z, bx, by, bz))
                s->y = highest_block(s->x, s->z) + 2;
        }
        if (sscanf(line, "L,%d,%d,%d,%d,%d,%d",
            &bp, &bq, &bx, &by, &bz, &bw) == 6)
            set_light(bp, bq, bx, by, bz, bw);

        if (sscanf(line, "P,%d,%f,%f,%f,%f,%f",
            &pid, &px, &py, &pz, &prx, &pry) == 6)
        {
            Player *player = find_player(pid);
            if (!player && g->player_count < MAX_PLAYERS) {
                player = g->players + g->player_count;
                g->player_count++;
                player->id = pid;
                player->buffer = 0;
                snprintf(player->name, MAX_NAME_LENGTH, "player%d", pid);
                update_player(player, px, py, pz, prx, pry, 1); // twice
            }
            if (player)
                update_player(player, px, py, pz, prx, pry, 1);
        }
        if (sscanf(line, "D,%d", &pid) == 1)
            delete_player(pid);


        if (sscanf(line, "K,%d,%d,%d", &kp, &kq, &kk) == 3)
            db_set_key(kp, kq, kk);

        if (sscanf(line, "R,%d,%d", &kp, &kq) == 2)
        {
            Chunk *chunk = find_chunk(kp, kq);
            if (chunk)
                dirty_chunk(chunk);
        }


        if (sscanf(line, "E,%lf,%d", &elapsed, &day_length) == 2)
        {
            glfwSetTime(fmod(elapsed, day_length));
            g->day_length = day_length;
            g->time_changed = 1;
        }
        if (line[0] == 'T' && line[1] == ',') {
            char *text = line + 2;
            add_message(text);
        }
        snprintf(
            format, sizeof(format), "N,%%d,%%%ds", MAX_NAME_LENGTH - 1);

        if (sscanf(line, format, &pid, name) == 2)
        {
            Player *player = find_player(pid);
            if (player)
                strncpy(player->name, name, MAX_NAME_LENGTH);
        }
        snprintf(
            format, sizeof(format),
            "S,%%d,%%d,%%d,%%d,%%d,%%d,%%%d[^\n]", MAX_SIGN_LENGTH - 1);
        if (sscanf(line, format,
            &bp, &bq, &bx, &by, &bz, &face, text) >= 6)
            _set_sign(bp, bq, bx, by, bz, face, text, 0);
        line = tokenize(NULL, "\n", &key);
    }
}

void reset_model(void)
{
   Model *g = (Model*)&model;

   memset(g->chunks, 0, sizeof(Chunk) * MAX_CHUNKS);
   g->chunk_count = 0;
   memset(g->players, 0, sizeof(Player) * MAX_PLAYERS);
   g->player_count = 0;
   g->observe1 = 0;
   g->observe2 = 0;
   g->flying = 0;
   g->item_index = 0;
   memset(g->typing_buffer, 0, sizeof(char) * MAX_TEXT_LENGTH);
   g->typing = 0;
   memset(g->messages, 0, sizeof(char) * MAX_MESSAGES * MAX_TEXT_LENGTH);
   g->message_index = 0;
   g->day_length = DAY_LENGTH;
   glfwSetTime(g->day_length / 3.0);
   g->time_changed = 1;
}

static craft_info_t info;

int main_init(void)
{
   // INITIALIZATION //
#ifdef HAVE_LIBCURL
   curl_global_init(CURL_GLOBAL_DEFAULT);
#endif
   srand(time(NULL));
   rand();

   return 0;
}

static void upload_texture_data(const unsigned char *in_data, size_t in_size,
      uintptr_t *tex, unsigned num)
{
   renderer_upload_texture_data(in_data, in_size, tex, num);
   load_png_texture_data(in_data, in_size);
}

int main_load_graphics(void)
{
   renderer_preinit();

   upload_texture_data((const unsigned char*)&tiles_texture[0], tiles_texture_length, &info.texture, 0);
   upload_texture_data((const unsigned char*)&font_texture[0],  font_texture_length,  &info.font,    1);
   upload_texture_data((const unsigned char*)&sky_texture[0],   sky_texture_length,   &info.sky,     2);
   upload_texture_data((const unsigned char*)&sign_texture[0],  sign_texture_length,  &info.sign,    3);

   renderer_load_shaders(&info);

   return 0;
}

int main_unload_graphics(void)
{
   renderer_free_texture(&info.texture);
   renderer_free_texture(&info.font);
   renderer_free_texture(&info.sky);
   renderer_free_texture(&info.sign);

   return 0;
}


int main_load_game(int argc, char **argv)
{
   int i;
   Model *g = (Model*)&model;

   main_load_graphics();

   // CHECK COMMAND LINE ARGUMENTS //
   if (argc == 2 || argc == 3)
   {

      g->mode = MODE_ONLINE;
      strncpy(g->server_addr, argv[1], MAX_ADDR_LENGTH);
      g->server_port = argc == 3 ? atoi(argv[2]) : DEFAULT_PORT;
      snprintf(g->db_path, MAX_PATH_LENGTH,
            "cache.%s.%d.db", g->server_addr, g->server_port);
   }
   else
   {
      g->mode = MODE_OFFLINE;
      main_set_db_path();
   }

   g->create_radius = CREATE_CHUNK_RADIUS;
   g->delete_radius = DELETE_CHUNK_RADIUS;
   g->sign_radius   = RENDER_SIGN_RADIUS;

   // INITIALIZE WORKER THREADS
   for (i = 0; i < WORKERS; i++) {
      Worker *worker = g->workers + i;
      worker->index = i;
      worker->state = WORKER_IDLE;
      mtx_init(&worker->mtx, mtx_plain);
      cnd_init(&worker->cnd);
      thrd_create(&worker->thrd, worker_run, worker);
   }

   // DATABASE INITIALIZATION //
   if (g->mode == MODE_OFFLINE || USE_CACHE)
   {
      db_enable();
      if (db_init(g->db_path))
         return -1;
      if (g->mode == MODE_ONLINE) {
         // TODO: support proper caching of signs (handle deletions)
         db_delete_all_signs();
      }
   }

   // CLIENT INITIALIZATION //
   if (g->mode == MODE_ONLINE) {
      client_enable();
      client_connect(g->server_addr, g->server_port);
      client_start();
      client_version(1);
      login();
   }

   // LOCAL VARIABLES //
   reset_model();
   info.fps.fps     = 0;
   info.fps.frames  = 0;
   info.fps.since   = 0;
   info.last_commit = glfwGetTime();
   info.last_update = glfwGetTime();
   info.sky_buffer = gen_sky_buffer();

   info.me = g->players;
   info.s = &g->players->state;
   info.me->id = 0;
   info.me->name[0] = '\0';
   info.me->buffer = 0;
   g->player_count = 1;

   {
      // LOAD STATE FROM DATABASE //
      int loaded = db_load_state(&info.s->x, &info.s->y, &info.s->z, &info.s->rx, &info.s->ry);
      force_chunks(info.me);
      if (!loaded)
         info.s->y = highest_block(info.s->x, info.s->z) + 2;

      // BEGIN MAIN LOOP //
      info.previous = glfwGetTime();
   }

   return 0;
}

void main_unload_game(void)
{
   main_unload_graphics();
#ifdef HAVE_LIBCURL
    curl_global_cleanup();
#endif
}

void main_deinit(void)
{
   db_save_state(info.s->x, info.s->y, info.s->z, info.s->rx, info.s->ry);
   db_close();
   db_disable();
   client_stop();
   client_disable();
   renderer_del_buffer(info.sky_buffer);
   delete_all_chunks();
   delete_all_players();
}

int main_run(void)
{
   int i;
   double now, dt;
   char *buffer;
   char text_buffer[1024];
   float ts, tx, ty;
   int face_count;
   Player *player;
   Model *g = (Model*)&model;
   // WINDOW SIZE AND SCALE //
   g->scale = get_scale_factor();
   g->width  = game_width;
   g->height = game_height;
   renderer_set_viewport(0, 0, g->width, g->height);

   // FRAME RATE //
   if (g->time_changed) {
      g->time_changed = 0;
      info.last_commit = glfwGetTime();
      info.last_update = glfwGetTime();
      memset(&info.fps, 0, sizeof(info.fps));
   }
   update_fps(&info.fps);
   now = glfwGetTime();
   dt = now - info.previous;
   dt = MIN(dt, 0.2);
   dt = MAX(dt, 0.0);
   info.previous = now;

   // HANDLE MOUSE INPUT //
   handle_mouse_input();

   // HANDLE MOVEMENT //
   handle_movement(dt);

   // HANDLE DATA FROM SERVER //
   buffer = client_recv();
   if (buffer) {
      parse_buffer(buffer);
      free(buffer);
   }

   // FLUSH DATABASE //
   if (now - info.last_commit > COMMIT_INTERVAL) {
      info.last_commit = now;
      db_commit();
   }

   // SEND POSITION TO SERVER //
   if (now - info.last_update > 0.1) {
      info.last_update = now;
      client_position(info.s->x, info.s->y, info.s->z, info.s->rx, info.s->ry);
   }

   // PREPARE TO RENDER //

   if (g->observe1 != 0 && g->player_count != 0)
      g->observe1 = g->observe1 % g->player_count;
   if (g->observe2 != 0 && g->player_count != 0)
      g->observe2 = g->observe2 % g->player_count;
   delete_chunks();
   if (info.me)
   {
      if (info.me->buffer)
         renderer_del_buffer(info.me->buffer);
      info.me->buffer = gen_player_buffer(info.s->x, info.s->y, info.s->z, info.s->rx, info.s->ry);
   }

   for (i = 1; i < g->player_count; i++)
      interpolate_player(g->players + i);

   player = g->players + g->observe1;

   // RENDER 3-D SCENE //
   renderer_clear_backbuffer();
   renderer_clear_depthbuffer();
   render_sky(&info.sky_attrib, player, info.sky_buffer);
   renderer_clear_depthbuffer();
   face_count = render_chunks(&info.block_attrib, player);
   render_signs(&info.text_attrib, player);
   render_sign(&info.text_attrib, player);
   render_players(&info.block_attrib, player);
   if (SHOW_WIREFRAME)
      render_wireframe(&info.line_attrib, player);
   render_water(&info.water_attrib, player);

   // RENDER HUD //
   renderer_clear_depthbuffer();
   if (SHOW_CROSSHAIRS) {
      render_crosshairs(&info.line_attrib);
   }
   if (SHOW_ITEM) {
      render_item(&info.block_attrib);
   }

   // RENDER TEXT //
   ts = 12 * g->scale;
   tx = ts / 2;
   ty = g->height - ts;
   if (SHOW_INFO_TEXT) {
      int hour = time_of_day() * 24;
      char am_pm = hour < 12 ? 'a' : 'p';
      hour = hour % 12;
      hour = hour ? hour : 12;
      snprintf(
            text_buffer, 1024,
            "(%d, %d) (%.2f, %.2f, %.2f) [%d, %d, %d] %d%cm %dfps",
            chunked(info.s->x), chunked(info.s->z), info.s->x, info.s->y, info.s->z,
            g->player_count, g->chunk_count,
            face_count * 2, hour, am_pm, info.fps.fps);
      render_text(&info.text_attrib, ALIGN_LEFT, tx, ty, ts, text_buffer);
      ty -= ts * 2;
   }
   if (SHOW_CHAT_TEXT) {
      int i;
      for (i = 0; i < MAX_MESSAGES; i++) {
         int index = (g->message_index + i) % MAX_MESSAGES;
         if (strlen(g->messages[index])) {
            render_text(&info.text_attrib, ALIGN_LEFT, tx, ty, ts,
                  g->messages[index]);
            ty -= ts * 2;
         }
      }
   }
   if (g->typing) {
      snprintf(text_buffer, 1024, "> %s", g->typing_buffer);
      render_text(&info.text_attrib, ALIGN_LEFT, tx, ty, ts, text_buffer);
      ty -= ts * 2;
   }
   if (SHOW_PLAYER_NAMES) {
      Player *other;
      if (player != info.me) {
         render_text(&info.text_attrib, ALIGN_CENTER,
               g->width / 2, ts, ts, player->name);
      }
      other = player_crosshair(player);
      if (other) {
         render_text(&info.text_attrib, ALIGN_CENTER,
               g->width / 2, g->height / 2 - ts - 24, ts,
               other->name);
      }
   }

   // RENDER PICTURE IN PICTURE //
   if (g->observe2)
   {
      player = g->players + g->observe2;

      {
         int pw = 256 * g->scale;
         int ph = 256 * g->scale;
         int offset = 32 * g->scale;
         int pad = 3 * g->scale;
         int sw = pw + pad * 2;
         int sh = ph + pad * 2;

         renderer_enable_scissor_test();
         renderer_scissor(g->width - sw - offset + pad, offset - pad, sw, sh);
         renderer_clear_backbuffer();
         renderer_disable_scissor_test();
         renderer_clear_depthbuffer();
         renderer_set_viewport(g->width - pw - offset, offset, pw, ph);

         g->width = pw;
         g->height = ph;
         g->ortho = 0;
         g->fov = FIELD_OF_VIEW;

         render_sky(&info.sky_attrib, player, info.sky_buffer);
         renderer_clear_depthbuffer();
         render_chunks(&info.block_attrib, player);
         render_signs(&info.text_attrib, player);
         render_players(&info.block_attrib, player);
         renderer_clear_depthbuffer();
         if (SHOW_PLAYER_NAMES) {
            render_text(&info.text_attrib, ALIGN_CENTER,
                  pw / 2, ts, ts, player->name);
         }
      }
   }

   if (g->mode_changed)
   {
      g->mode_changed = 0;
      return 0;
   }

   return 1;
}
