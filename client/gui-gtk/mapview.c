/********************************************************************** 
 Freeciv - Copyright (C) 1996 - A Kjeldberg, L Gregersen, P Unold
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
***********************************************************************/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <assert.h>
#include <stdio.h>

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include <gtk/gtk.h>

#include "fcintl.h"
#include "game.h"
#include "government.h"		/* government_graphic() */
#include "log.h"
#include "map.h"
#include "player.h"
#include "rand.h"
#include "support.h"
#include "timing.h"

#include "civclient.h"
#include "climap.h"
#include "climisc.h"
#include "colors.h"
#include "control.h" /* set_unit_focus_no_center and get_unit_in_focus */
#include "goto.h"
#include "graphics.h"
#include "gui_main.h"
#include "gui_stuff.h"
#include "mapctrl.h"
#include "options.h"
#include "tilespec.h"

#include "citydlg.h" /* For reset_city_dialogs() */
#include "mapview.h"

static void pixmap_put_overlay_tile(GdkDrawable *pixmap,
				    int canvas_x, int canvas_y,
				    struct Sprite *ssprite);
static void put_overlay_tile_gpixmap(GtkPixcomm *p,
				     int canvas_x, int canvas_y,
				     struct Sprite *ssprite);
static void put_unit_pixmap(struct unit *punit, GdkPixmap *pm,
			    int canvas_x, int canvas_y);
static void put_line(GdkDrawable *pm, int x, int y, int dir);

static void put_unit_pixmap_draw(struct unit *punit, GdkPixmap *pm,
				 int canvas_x, int canvas_y,
				 int offset_x, int offset_y_unit,
				 int width, int height_unit);
static void pixmap_put_overlay_tile_draw(GdkDrawable *pixmap,
					 int canvas_x, int canvas_y,
					 struct Sprite *ssprite,
					 int offset_x, int offset_y,
					 int width, int height,
					 int fog);
static void really_draw_segment(int src_x, int src_y, int dir,
				bool write_to_screen, bool force);
static void pixmap_put_tile_iso(GdkDrawable *pm, int x, int y,
				int canvas_x, int canvas_y,
				int citymode,
				int offset_x, int offset_y, int offset_y_unit,
				int width, int height, int height_unit,
				enum draw_type draw);
static void pixmap_put_black_tile_iso(GdkDrawable *pm,
				      int canvas_x, int canvas_y,
				      int offset_x, int offset_y,
				      int width, int height);

/* the intro picture is held in this pixmap, which is scaled to
   the screen size */
static SPRITE *scaled_intro_sprite = NULL;

static GtkObject *map_hadj, *map_vadj;


/**************************************************************************
 This function is called to decrease a unit's HP smoothly in battle
 when combat_animation is turned on.
**************************************************************************/
void decrease_unit_hp_smooth(struct unit *punit0, int hp0, 
			     struct unit *punit1, int hp1)
{
  static struct timer *anim_timer = NULL; 
  struct unit *losing_unit = (hp0 == 0 ? punit0 : punit1);
  int i;

  set_units_in_combat(punit0, punit1);

  do {
    anim_timer = renew_timer_start(anim_timer, TIMER_USER, TIMER_ACTIVE);

    if (punit0->hp > hp0
	&& myrand((punit0->hp - hp0) + (punit1->hp - hp1)) < punit0->hp - hp0)
      punit0->hp--;
    else if (punit1->hp > hp1)
      punit1->hp--;
    else
      punit0->hp--;

    refresh_tile_mapcanvas(punit0->x, punit0->y, TRUE);
    refresh_tile_mapcanvas(punit1->x, punit1->y, TRUE);

    gdk_flush();
    usleep_since_timer_start(anim_timer, 10000);

  } while (punit0->hp > hp0 || punit1->hp > hp1);

  for (i = 0; i < num_tiles_explode_unit; i++) {
    int canvas_x, canvas_y;
    get_canvas_xy(losing_unit->x, losing_unit->y, &canvas_x, &canvas_y);
    anim_timer = renew_timer_start(anim_timer, TIMER_USER, TIMER_ACTIVE);
    if (is_isometric) {
      /* We first draw the explosion onto the unit and draw draw the
	 complete thing onto the map canvas window. This avoids flickering. */
      gdk_draw_pixmap(single_tile_pixmap, civ_gc, map_canvas_store,
		      canvas_x, canvas_y,
		      0, 0,
		      NORMAL_TILE_WIDTH, NORMAL_TILE_HEIGHT);
      pixmap_put_overlay_tile(single_tile_pixmap,
			      NORMAL_TILE_WIDTH/4, 0,
			      sprites.explode.unit[i]);
      gdk_draw_pixmap(map_canvas->window, civ_gc, single_tile_pixmap,
		      0, 0,
		      canvas_x, canvas_y,
		      NORMAL_TILE_WIDTH, NORMAL_TILE_HEIGHT);
    } else { /* is_isometric */
      /* FIXME: maybe do as described in the above comment. */
      struct canvas_store store = {single_tile_pixmap};

      put_one_tile(&store, losing_unit->x, losing_unit->y,
		   0, 0, FALSE);
      put_unit_pixmap(losing_unit, single_tile_pixmap, 0, 0);
      pixmap_put_overlay_tile(single_tile_pixmap, 0, 0,
			      sprites.explode.unit[i]);

      gdk_draw_pixmap(map_canvas->window, civ_gc, single_tile_pixmap,
		      0, 0,
		      canvas_x, canvas_y,
		      UNIT_TILE_WIDTH,
		      UNIT_TILE_HEIGHT);
    }
    gdk_flush();
    usleep_since_timer_start(anim_timer, 20000);
  }

  set_units_in_combat(NULL, NULL);
  refresh_tile_mapcanvas(punit0->x, punit0->y, TRUE);
  refresh_tile_mapcanvas(punit1->x, punit1->y, TRUE);
}

/**************************************************************************
  If do_restore is FALSE it will invert the turn done button style. If
  called regularly from a timer this will give a blinking turn done
  button. If do_restore is TRUE this will reset the turn done button
  to the default style.
**************************************************************************/
void update_turn_done_button(bool do_restore)
{
  static bool flip = FALSE;
  
  if (!get_turn_done_button_state()) {
    return;
  }

  if ((do_restore && flip) || !do_restore) {
    GdkGC *fore = turn_done_button->style->bg_gc[GTK_STATE_NORMAL];
    GdkGC *back = turn_done_button->style->light_gc[GTK_STATE_NORMAL];

    turn_done_button->style->bg_gc[GTK_STATE_NORMAL] = back;
    turn_done_button->style->light_gc[GTK_STATE_NORMAL] = fore;

    gtk_expose_now(turn_done_button);

    flip = !flip;
  }
}

/**************************************************************************
...
**************************************************************************/
void update_timeout_label(void)
{
  char buffer[512];

  if (game.timeout <= 0)
    sz_strlcpy(buffer, Q_("?timeout:off"));
  else
    format_duration(buffer, sizeof(buffer), seconds_to_turndone);
  gtk_set_label(timeout_label, buffer);
}

/**************************************************************************
...
**************************************************************************/
void update_info_label( void )
{
  char buffer	[512];
  int  d;

  gtk_frame_set_label( GTK_FRAME( main_frame_civ_name ), get_nation_name(game.player_ptr->nation) );

  my_snprintf(buffer, sizeof(buffer),
	      _("Population: %s\nYear: %s\n"
		"Gold %d\nTax: %d Lux: %d Sci: %d"),
	      population_to_text(civ_population(game.player_ptr)),
	      textyear(game.year), game.player_ptr->economic.gold,
	      game.player_ptr->economic.tax,
	      game.player_ptr->economic.luxury,
	      game.player_ptr->economic.science);

  gtk_set_label(main_label_info, buffer);

  set_indicator_icons(client_research_sprite(),
		      client_warming_sprite(),
		      client_cooling_sprite(),
		      game.player_ptr->government);

  d=0;
  for (; d < game.player_ptr->economic.luxury /10; d++) {
    struct Sprite *sprite = get_citizen_sprite(CITIZEN_ELVIS, d, NULL);
    gtk_pixmap_set(GTK_PIXMAP(econ_label[d]), sprite->pixmap, sprite->mask);
  }
 
  for (; d < (game.player_ptr->economic.science
	     + game.player_ptr->economic.luxury) / 10; d++) {
    struct Sprite *sprite = get_citizen_sprite(CITIZEN_SCIENTIST, d, NULL);
    gtk_pixmap_set(GTK_PIXMAP(econ_label[d]), sprite->pixmap, sprite->mask);
  }
 
  for (; d < 10; d++) {
    struct Sprite *sprite = get_citizen_sprite(CITIZEN_TAXMAN, d, NULL);
    gtk_pixmap_set(GTK_PIXMAP(econ_label[d]), sprite->pixmap, sprite->mask);
  }
 
  update_timeout_label();
}

/**************************************************************************
  Update the information label which gives info on the current unit and the
  square under the current unit, for specified unit.  Note that in practice
  punit is always the focus unit.
  Clears label if punit is NULL.
  Also updates the cursor for the map_canvas (this is related because the
  info label includes a "select destination" prompt etc).
  Also calls update_unit_pix_label() to update the icons for units on this
  square.
**************************************************************************/
void update_unit_info_label(struct unit *punit)
{
  if (punit && get_client_state() != CLIENT_GAME_OVER_STATE) {
    char buffer[512];
    struct city *pcity =
	player_find_city_by_id(game.player_ptr, punit->homecity);
    int infrastructure =
	get_tile_infrastructure_set(map_get_tile(punit->x, punit->y));

    my_snprintf(buffer, sizeof(buffer), "%s %s", 
            unit_type(punit)->name,
            (punit->veteran) ? _("(veteran)") : "" );
    gtk_frame_set_label( GTK_FRAME(unit_info_frame), buffer);


    my_snprintf(buffer, sizeof(buffer), "%s\n%s\n%s%s%s",
		(hover_unit == punit->id) ?
		_("Select destination") : unit_activity_text(punit),
		map_get_tile_info_text(punit->x, punit->y),
		infrastructure ?
		map_get_infrastructure_text(infrastructure) : "",
		infrastructure ? "\n" : "", pcity ? pcity->name : "");
    gtk_set_label( unit_info_label, buffer);

    if (hover_unit != punit->id)
      set_hover_state(NULL, HOVER_NONE);

    switch (hover_state) {
    case HOVER_NONE:
      gdk_window_set_cursor (root_window, NULL);
      break;
    case HOVER_PATROL:
      gdk_window_set_cursor (root_window, patrol_cursor);
      break;
    case HOVER_GOTO:
    case HOVER_CONNECT:
      gdk_window_set_cursor (root_window, goto_cursor);
      break;
    case HOVER_NUKE:
      gdk_window_set_cursor (root_window, nuke_cursor);
      break;
    case HOVER_PARADROP:
      gdk_window_set_cursor (root_window, drop_cursor);
      break;
    }
  } else {
    gtk_frame_set_label( GTK_FRAME(unit_info_frame),"");
    gtk_set_label(unit_info_label,"\n\n");
    gdk_window_set_cursor(root_window, NULL);
  }
  update_unit_pix_label(punit);
}


/**************************************************************************
...
**************************************************************************/
GdkPixmap *get_thumb_pixmap(int onoff)
{
  return sprites.treaty_thumb[BOOL_VAL(onoff)]->pixmap;
}

/**************************************************************************
...
**************************************************************************/
void set_indicator_icons(int bulb, int sol, int flake, int gov)
{
  struct Sprite *gov_sprite;

  bulb = CLIP(0, bulb, NUM_TILES_PROGRESS-1);
  sol = CLIP(0, sol, NUM_TILES_PROGRESS-1);
  flake = CLIP(0, flake, NUM_TILES_PROGRESS-1);

  gtk_pixmap_set(GTK_PIXMAP(bulb_label), sprites.bulb[bulb]->pixmap, NULL);
  gtk_pixmap_set(GTK_PIXMAP(sun_label), sprites.warming[sol]->pixmap, NULL);
  gtk_pixmap_set(GTK_PIXMAP(flake_label), sprites.cooling[flake]->pixmap, NULL);

  if (game.government_count==0) {
    /* not sure what to do here */
    gov_sprite = get_citizen_sprite(CITIZEN_UNHAPPY, 0, NULL); 
  } else {
    gov_sprite = get_government(gov)->sprite;
  }
  gtk_pixmap_set(GTK_PIXMAP(government_label), gov_sprite->pixmap, NULL);
}

/**************************************************************************
  Draw a single frame of animation.  This function needs to clear the old
  image and draw the new one.  It must flush output to the display.
**************************************************************************/
void draw_unit_animation_frame(struct unit *punit,
			       bool first_frame, bool last_frame,
			       int old_canvas_x, int old_canvas_y,
			       int new_canvas_x, int new_canvas_y)
{
  /* Clear old sprite. */
  gdk_draw_pixmap(map_canvas->window, civ_gc, map_canvas_store, old_canvas_x,
		  old_canvas_y, old_canvas_x, old_canvas_y, UNIT_TILE_WIDTH,
		  UNIT_TILE_HEIGHT);

  /* Draw the new sprite. */
  gdk_draw_pixmap(single_tile_pixmap, civ_gc, map_canvas_store, new_canvas_x,
		  new_canvas_y, 0, 0, UNIT_TILE_WIDTH, UNIT_TILE_HEIGHT);
  put_unit_pixmap(punit, single_tile_pixmap, 0, 0);

  /* Write to screen. */
  gdk_draw_pixmap(map_canvas->window, civ_gc, single_tile_pixmap, 0, 0,
		  new_canvas_x, new_canvas_y, UNIT_TILE_WIDTH,
		  UNIT_TILE_HEIGHT);

  /* Flush. */
  gdk_flush();
}

/**************************************************************************
...
**************************************************************************/
void set_overview_dimensions(int x, int y)
{
  overview_canvas_store_width=2*x;
  overview_canvas_store_height=2*y;

  if (overview_canvas_store)
    gdk_pixmap_unref(overview_canvas_store);
  
  overview_canvas_store	= gdk_pixmap_new(root_window,
			  overview_canvas_store_width,
			  overview_canvas_store_height, -1);

  gdk_gc_set_foreground(fill_bg_gc, colors_standard[COLOR_STD_BLACK]);
  gdk_draw_rectangle(overview_canvas_store, fill_bg_gc, TRUE,
		     0, 0,
		     overview_canvas_store_width, overview_canvas_store_height);

  gtk_widget_set_usize(overview_canvas, 2*x, 2*y);
  update_map_canvas_scrollbars_size();
}

/**************************************************************************
...
**************************************************************************/
gint overview_canvas_expose(GtkWidget *w, GdkEventExpose *ev)
{
  if (!can_client_change_view()) {
    if(radar_gfx_sprite)
      gdk_draw_pixmap(overview_canvas->window, civ_gc,
		      radar_gfx_sprite->pixmap, ev->area.x, ev->area.y,
		      ev->area.x, ev->area.y, ev->area.width, ev->area.height);
    return TRUE;
  }
  
  refresh_overview_viewrect();
  return TRUE;
}

/**************************************************************************
...
**************************************************************************/
static void set_overview_tile_foreground_color(int x, int y)
{
  gdk_gc_set_foreground(fill_bg_gc,
			colors_standard[overview_tile_color(x, y)]);
}

/**************************************************************************
...
**************************************************************************/
void refresh_overview_canvas(void)
{
  whole_map_iterate(x, y) {
    set_overview_tile_foreground_color(x, y);
    gdk_draw_rectangle(overview_canvas_store, fill_bg_gc, TRUE, x * 2,
		       y * 2, 2, 2);
  } whole_map_iterate_end;

  gdk_gc_set_foreground( fill_bg_gc, colors_standard[COLOR_STD_BLACK] );
}


/**************************************************************************
...
**************************************************************************/
void overview_update_tile(int x, int y)
{
  int screen_width, pos;

  if (is_isometric) {
    screen_width = map_canvas_store_twidth + map_canvas_store_theight;
  } else {
    screen_width = map_canvas_store_twidth;
  }
  pos = x + map.xsize/2 - (map_view_x0 + screen_width/2);
  
  pos %= map.xsize;
  if (pos < 0)
    pos += map.xsize;
  
  set_overview_tile_foreground_color(x, y);
  gdk_draw_rectangle(overview_canvas_store, fill_bg_gc, TRUE, x*2, y*2,
		     2, 2);
  
  gdk_draw_rectangle(overview_canvas->window, fill_bg_gc, TRUE, pos*2, y*2,
		     2, 2);
}

/**************************************************************************
...
**************************************************************************/
void refresh_overview_viewrect(void)
{
  int screen_width, delta;
  if (is_isometric) {
    screen_width = map_canvas_store_twidth + map_canvas_store_theight;
  } else {
    screen_width = map_canvas_store_twidth;
  }
  delta = map.xsize/2 - (map_view_x0 + screen_width/2);

  if (delta>=0) {
    gdk_draw_pixmap( overview_canvas->window, civ_gc, overview_canvas_store,
		0, 0, 2*delta, 0,
		overview_canvas_store_width-2*delta,
		overview_canvas_store_height );
    gdk_draw_pixmap( overview_canvas->window, civ_gc, overview_canvas_store,
		overview_canvas_store_width-2*delta, 0,
		0, 0,
		2*delta, overview_canvas_store_height );
  } else {
    gdk_draw_pixmap( overview_canvas->window, civ_gc, overview_canvas_store,
		-2*delta, 0,
		0, 0,
		overview_canvas_store_width+2*delta,
		overview_canvas_store_height );

    gdk_draw_pixmap( overview_canvas->window, civ_gc, overview_canvas_store,
		0, 0,
		overview_canvas_store_width+2*delta, 0,
		-2*delta, overview_canvas_store_height );
  }

  gdk_gc_set_foreground( civ_gc, colors_standard[COLOR_STD_WHITE] );
  
  if (is_isometric) {
    /* The x's and y's are in overview coordinates.
       All the extra factor 2's are because one tile in the overview
       is 2x2 pixels. */
    int Wx = overview_canvas_store_width/2 - screen_width /* *2/2 */;
    int Wy = map_view_y0 * 2;
    int Nx = Wx + 2 * map_canvas_store_twidth;
    int Ny = Wy - 2 * map_canvas_store_twidth;
    int Sx = Wx + 2 * map_canvas_store_theight;
    int Sy = Wy + 2 * map_canvas_store_theight;
    int Ex = Nx + 2 * map_canvas_store_theight;
    int Ey = Ny + 2 * map_canvas_store_theight;
    
    freelog(LOG_DEBUG, "wx,wy: %d,%d nx,ny:%d,%x ex,ey:%d,%d, sx,sy:%d,%d",
	    Wx, Wy, Nx, Ny, Ex, Ey, Sx, Sy);

    /* W to N */
    gdk_draw_line(overview_canvas->window, civ_gc,
		  Wx, Wy, Nx, Ny);

    /* N to E */
    gdk_draw_line(overview_canvas->window, civ_gc,
		  Nx, Ny, Ex, Ey);

    /* E to S */
    gdk_draw_line(overview_canvas->window, civ_gc,
		  Ex, Ey, Sx, Sy);

    /* S to W */
    gdk_draw_line(overview_canvas->window, civ_gc,
		  Sx, Sy, Wx, Wy);
  } else {
    gdk_draw_rectangle(overview_canvas->window, civ_gc, FALSE,
		       (overview_canvas_store_width-2*map_canvas_store_twidth)/2,
		       2*map_view_y0,
		       2*map_canvas_store_twidth, 2*map_canvas_store_theight-1);
  }
}

/**************************************************************************
...
**************************************************************************/
gint map_canvas_expose(GtkWidget *w, GdkEventExpose *ev)
{
  gint height, width;
  int tile_width, tile_height;
  gboolean map_resized;
  static int exposed_once = 0;

  gdk_window_get_size(w->window, &width, &height);

  mapview_canvas.width = width;
  mapview_canvas.height = height;

  tile_width=(width+NORMAL_TILE_WIDTH-1)/NORMAL_TILE_WIDTH;
  tile_height=(height+NORMAL_TILE_HEIGHT-1)/NORMAL_TILE_HEIGHT;

  map_resized=FALSE;
  if(map_canvas_store_twidth !=tile_width ||
     map_canvas_store_theight!=tile_height) { /* resized? */
    gdk_pixmap_unref(map_canvas_store);
  
    map_canvas_store_twidth=tile_width;
    map_canvas_store_theight=tile_height;
/*
    gtk_drawing_area_size(GTK_DRAWING_AREA(map_canvas),
  		    map_canvas_store_twidth,
  		    map_canvas_store_theight);
*/
    map_canvas_store= gdk_pixmap_new( map_canvas->window,
  		    tile_width*NORMAL_TILE_WIDTH,
  		    tile_height*NORMAL_TILE_HEIGHT,
  		    -1 );
    mapview_canvas.store->pixmap = map_canvas_store;

    gdk_gc_set_foreground(fill_bg_gc, colors_standard[COLOR_STD_BLACK]);
    gdk_draw_rectangle(map_canvas_store, fill_bg_gc, TRUE,
		       0, 0,
		       NORMAL_TILE_WIDTH*map_canvas_store_twidth,
		       NORMAL_TILE_HEIGHT*map_canvas_store_theight);
    update_map_canvas_scrollbars_size();
    map_resized=TRUE;
  }

  if (!can_client_change_view()) {
    if (!intro_gfx_sprite) {
      load_intro_gfx();
    }
    if (!scaled_intro_sprite || height != scaled_intro_sprite->height
	|| width != scaled_intro_sprite->width) {
      if (scaled_intro_sprite) {
	free_sprite(scaled_intro_sprite);
      }
	
      scaled_intro_sprite = sprite_scale(intro_gfx_sprite, width, height);
    }
    
    if (scaled_intro_sprite) {
      gdk_draw_pixmap(map_canvas->window, civ_gc,
		      scaled_intro_sprite->pixmap, ev->area.x,
		      ev->area.y, ev->area.x, ev->area.y,
		      ev->area.width, ev->area.height);
    }
  }
  else
  {
    if (scaled_intro_sprite) {
      free_sprite(scaled_intro_sprite);
      scaled_intro_sprite = NULL;
    }

    if(map.xsize) { /* do we have a map at all */
      if(map_resized) {
	update_map_canvas_visible();

	update_map_canvas_scrollbars();

    	refresh_overview_viewrect();
      }
      else {
	gdk_draw_pixmap( map_canvas->window, civ_gc, map_canvas_store,
		ev->area.x, ev->area.y, ev->area.x, ev->area.y,
		ev->area.width, ev->area.height );
      }
    }
    refresh_overview_canvas();
  }

  if (!exposed_once) {
    center_on_something();
    exposed_once = 1;
  }
  return TRUE;
}

/**************************************************************************
...
**************************************************************************/
void pixmap_put_black_tile(GdkDrawable *pm,
			   int canvas_x, int canvas_y)
{
  gdk_gc_set_foreground( fill_bg_gc, colors_standard[COLOR_STD_BLACK] );
  gdk_draw_rectangle(pm, fill_bg_gc, TRUE,
		     canvas_x, canvas_y,
		     NORMAL_TILE_WIDTH, NORMAL_TILE_HEIGHT);
}

/**************************************************************************
FIXME: Find a better way to put flags and such on top.
**************************************************************************/
static void put_unit_pixmap(struct unit *punit, GdkPixmap *pm,
			    int canvas_x, int canvas_y)
{
  int solid_bg;

  if (is_isometric) {
    struct Sprite *sprites[40];
    int count = fill_unit_sprite_array(sprites, punit, &solid_bg);
    int i;

    assert(!solid_bg);
    for (i=0; i<count; i++) {
      if (sprites[i]) {
	pixmap_put_overlay_tile(pm, canvas_x, canvas_y, sprites[i]);
      }
    }
  } else { /* is_isometric */
    struct Sprite *sprites[40];
    int count = fill_unit_sprite_array(sprites, punit, &solid_bg);

    if (count) {
      int i = 0;

      if (solid_bg) {
	gdk_gc_set_foreground(fill_bg_gc,
			      colors_standard[player_color(unit_owner(punit))]);
	gdk_draw_rectangle(pm, fill_bg_gc, TRUE,
			   canvas_x, canvas_y,
			   UNIT_TILE_WIDTH, UNIT_TILE_HEIGHT);
      } else {
	pixmap_put_overlay_tile(pm, canvas_x, canvas_y, sprites[0]);
	i++;
      }

      for (; i<count; i++) {
	if (sprites[i])
	  pixmap_put_overlay_tile(pm, canvas_x, canvas_y, sprites[i]);
      }
    }
  }
}

/**************************************************************************
Only used for isometric view.
**************************************************************************/
static void put_unit_pixmap_draw(struct unit *punit, GdkPixmap *pm,
				 int canvas_x, int canvas_y,
				 int offset_x, int offset_y_unit,
				 int width, int height_unit)
{
  struct Sprite *sprites[40];
  int dummy;
  int count = fill_unit_sprite_array(sprites, punit, &dummy);
  int i;

  for (i=0; i<count; i++) {
    if (sprites[i]) {
      pixmap_put_overlay_tile_draw(pm, canvas_x, canvas_y, sprites[i],
				   offset_x, offset_y_unit,
				   width, height_unit, 0);
    }
  }
}

/**************************************************************************
Only used for isometric view.
**************************************************************************/
void put_one_tile_full(GdkDrawable *pm, int x, int y,
		       int canvas_x, int canvas_y, int citymode)
{
  pixmap_put_tile_iso(pm, x, y, canvas_x, canvas_y, citymode,
		      0, 0, 0,
		      NORMAL_TILE_WIDTH, NORMAL_TILE_HEIGHT, UNIT_TILE_HEIGHT,
		      D_FULL);
}

/**************************************************************************
  Draw some or all of a tile onto the mapview canvas.
**************************************************************************/
void gui_map_put_tile_iso(int map_x, int map_y,
			  int canvas_x, int canvas_y,
			  int offset_x, int offset_y, int offset_y_unit,
			  int width, int height, int height_unit,
			  enum draw_type draw)
{
  pixmap_put_tile_iso(map_canvas_store,
		      map_x, map_y, canvas_x, canvas_y,
		      FALSE,
		      offset_x, offset_y, offset_y_unit,
		      width, height, height_unit, draw);
}

/**************************************************************************
  Flush the given part of the canvas buffer (if there is one) to the
  screen.
**************************************************************************/
void flush_mapcanvas(int canvas_x, int canvas_y,
		     int pixel_width, int pixel_height)
{
  gdk_draw_pixmap(map_canvas->window, civ_gc, map_canvas_store,
		  canvas_x, canvas_y, canvas_x, canvas_y,
		  pixel_width, pixel_height);
}

/**************************************************************************
 Update display of descriptions associated with cities on the main map.
**************************************************************************/
void update_city_descriptions(void)
{
  update_map_canvas_visible();
}

/**************************************************************************
  If necessary, clear the city descriptions out of the buffer.
**************************************************************************/
void prepare_show_city_descriptions(void)
{
  /* Nothing to do */
}

/**************************************************************************
...
**************************************************************************/
void show_city_desc(struct city *pcity, int canvas_x, int canvas_y)
{
  static char buffer[512], buffer2[32];
  int w, w2, ascent;
  enum color_std color;

  canvas_x += NORMAL_TILE_WIDTH / 2;
  canvas_y += NORMAL_TILE_HEIGHT;

  get_city_mapview_name_and_growth(pcity, buffer, sizeof(buffer),
				   buffer2, sizeof(buffer2), &color);

  gdk_string_extents(main_fontset, buffer, NULL, NULL, &w, &ascent, NULL);
  if (buffer2[0] != '\0') {
    /* HACK: put a character's worth of space between the two strings. */
    w += gdk_string_width(main_fontset, "M");
  }
  w2 = gdk_string_width(prod_fontset, buffer2);

  gtk_draw_shadowed_string(map_canvas_store, main_fontset,
			   toplevel->style->black_gc,
			   toplevel->style->white_gc,
			   canvas_x - (w + w2) / 2,
			   canvas_y + ascent,
			   buffer);
  gdk_gc_set_foreground(civ_gc, colors_standard[color]);
  gtk_draw_shadowed_string(map_canvas_store, prod_fontset,
			   toplevel->style->black_gc,
			   civ_gc,
			   canvas_x - (w + w2) / 2 + w,
			   canvas_y + ascent,
			   buffer2);

  if (draw_city_productions && (pcity->owner==game.player_idx)) {
    if (draw_city_names) {
      canvas_y += gdk_string_height(main_fontset, buffer);
    }

    get_city_mapview_production(pcity, buffer, sizeof(buffer));

    gdk_string_extents(prod_fontset, buffer, NULL, NULL, &w, &ascent, NULL);
    gtk_draw_shadowed_string(map_canvas_store, prod_fontset,
			     toplevel->style->black_gc,
			     toplevel->style->white_gc, canvas_x - w / 2,
			     canvas_y + ascent + 3, buffer);
  }
}

/**************************************************************************
...
**************************************************************************/
void put_city_tile_output(GdkDrawable *pm, int canvas_x, int canvas_y, 
			  int food, int shield, int trade)
{
  food = CLIP(0, food, NUM_TILES_DIGITS-1);
  trade = CLIP(0, trade, NUM_TILES_DIGITS-1);
  shield = CLIP(0, shield, NUM_TILES_DIGITS-1);
  
  if (is_isometric) {
    canvas_x += NORMAL_TILE_WIDTH/3;
    canvas_y -= NORMAL_TILE_HEIGHT/3;
  }

  pixmap_put_overlay_tile(pm, canvas_x, canvas_y,
			  sprites.city.tile_foodnum[food]);
  pixmap_put_overlay_tile(pm, canvas_x, canvas_y,
			  sprites.city.tile_shieldnum[shield]);
  pixmap_put_overlay_tile(pm, canvas_x, canvas_y,
			  sprites.city.tile_tradenum[trade]);
}

/**************************************************************************
...
**************************************************************************/
void put_unit_gpixmap(struct unit *punit, GtkPixcomm *p)
{
  struct Sprite *sprites[40];
  int solid_bg;
  int count = fill_unit_sprite_array(sprites, punit, &solid_bg);

  if (count) {
    int i;

    if (solid_bg) {
      gtk_pixcomm_fill(p, colors_standard[player_color(unit_owner(punit))],
		       FALSE);
    }

    for (i=0;i<count;i++) {
      if (sprites[i])
        put_overlay_tile_gpixmap(p, 0, 0, sprites[i]);
    }
  }

  gtk_pixcomm_changed(GTK_PIXCOMM(p));
}


/**************************************************************************
  FIXME:
  For now only two food, one shield and two masks can be drawn per unit,
  the proper way to do this is probably something like what Civ II does.
  (One food/shield/mask drawn N times, possibly one top of itself. -- SKi 
**************************************************************************/
void put_unit_gpixmap_city_overlays(struct unit *punit, GtkPixcomm *p)
{
  int upkeep_food = CLIP(0, punit->upkeep_food, 2);
  int unhappy = CLIP(0, punit->unhappiness, 2);
 
  /* draw overlay pixmaps */
  if (punit->upkeep > 0)
    put_overlay_tile_gpixmap(p, 0, NORMAL_TILE_HEIGHT, sprites.upkeep.shield);
  if (upkeep_food > 0)
    put_overlay_tile_gpixmap(p, 0, NORMAL_TILE_HEIGHT, sprites.upkeep.food[upkeep_food-1]);
  if (unhappy > 0)
    put_overlay_tile_gpixmap(p, 0, NORMAL_TILE_HEIGHT, sprites.upkeep.unhappy[unhappy-1]);
}

/**************************************************************************
...
**************************************************************************/
void put_nuke_mushroom_pixmaps(int x, int y)
{
  if (is_isometric) {
    int canvas_x, canvas_y;
    struct Sprite *mysprite = sprites.explode.iso_nuke;

    get_canvas_xy(x, y, &canvas_x, &canvas_y);
    canvas_x += NORMAL_TILE_WIDTH/2 - mysprite->width/2;
    canvas_y += NORMAL_TILE_HEIGHT/2 - mysprite->height/2;

    pixmap_put_overlay_tile(map_canvas->window, canvas_x, canvas_y,
			    mysprite);

    gdk_flush();
    sleep(1);

    update_map_canvas_visible();
  } else {
    int x_itr, y_itr;
    int canvas_x, canvas_y;

    for (y_itr=0; y_itr<3; y_itr++) {
      for (x_itr=0; x_itr<3; x_itr++) {
	struct Sprite *mysprite = sprites.explode.nuke[y_itr][x_itr];
	get_canvas_xy(x + x_itr - 1, y + y_itr - 1, &canvas_x, &canvas_y);

	gdk_draw_pixmap(single_tile_pixmap, civ_gc, map_canvas_store,
			canvas_x, canvas_y, 0, 0,
			NORMAL_TILE_WIDTH, NORMAL_TILE_HEIGHT);
	pixmap_put_overlay_tile(single_tile_pixmap, 0, 0, mysprite);
	gdk_draw_pixmap(map_canvas->window, civ_gc, single_tile_pixmap,
			0, 0, canvas_x, canvas_y,
			NORMAL_TILE_WIDTH, NORMAL_TILE_HEIGHT);
      }
    }

    gdk_flush();
    sleep(1);

    update_map_canvas(x-1, y-1, 3, 3, TRUE);
  }
}

/**************************************************************************
canvas_x, canvas_y is the top left corner of the pixmap.
**************************************************************************/
void pixmap_frame_tile_red(GdkDrawable *pm,
			   int canvas_x, int canvas_y)
{
  if (is_isometric) {
    gdk_gc_set_foreground(thick_line_gc, colors_standard[COLOR_STD_RED]);

    gdk_draw_line(pm, thick_line_gc,
		  canvas_x+NORMAL_TILE_WIDTH/2-1, canvas_y,
		  canvas_x+NORMAL_TILE_WIDTH-1, canvas_y+NORMAL_TILE_HEIGHT/2-1);
    gdk_draw_line(pm, thick_line_gc,
		  canvas_x+NORMAL_TILE_WIDTH-1, canvas_y+NORMAL_TILE_HEIGHT/2-1,
		  canvas_x+NORMAL_TILE_WIDTH/2-1, canvas_y+NORMAL_TILE_HEIGHT-1);
    gdk_draw_line(pm, thick_line_gc,
		  canvas_x+NORMAL_TILE_WIDTH/2-1, canvas_y+NORMAL_TILE_HEIGHT-1,
		  canvas_x, canvas_y + NORMAL_TILE_HEIGHT/2-1);
    gdk_draw_line(pm, thick_line_gc,
		  canvas_x, canvas_y + NORMAL_TILE_HEIGHT/2-1,
		  canvas_x+NORMAL_TILE_WIDTH/2-1, canvas_y);
  } else {
    gdk_gc_set_foreground(fill_bg_gc, colors_standard[COLOR_STD_RED]);

    gdk_draw_rectangle(pm, fill_bg_gc, FALSE,
		       canvas_x, canvas_y,
		       NORMAL_TILE_WIDTH-1, NORMAL_TILE_HEIGHT-1);
  }
}

/**************************************************************************
...
**************************************************************************/
static void put_overlay_tile_gpixmap(GtkPixcomm *p, int canvas_x, int canvas_y,
				     struct Sprite *ssprite)
{
  if (!ssprite)
    return;

  gtk_pixcomm_copyto (p, ssprite, canvas_x, canvas_y,
		FALSE);
}

/**************************************************************************
...
**************************************************************************/
static void pixmap_put_overlay_tile(GdkDrawable *pixmap,
				    int canvas_x, int canvas_y,
				    struct Sprite *ssprite)
{
  if (!ssprite)
    return;
      
  gdk_gc_set_clip_origin(civ_gc, canvas_x, canvas_y);
  gdk_gc_set_clip_mask(civ_gc, ssprite->mask);

  gdk_draw_pixmap(pixmap, civ_gc, ssprite->pixmap,
		  0, 0,
		  canvas_x, canvas_y,
		  ssprite->width, ssprite->height);
  gdk_gc_set_clip_mask(civ_gc, NULL);
}

/**************************************************************************
  Place part of a (possibly masked) sprite on a pixmap.
**************************************************************************/
static void pixmap_put_sprite(GdkDrawable *pixmap,
			      int pixmap_x, int pixmap_y,
			      struct Sprite *ssprite,
			      int offset_x, int offset_y,
			      int width, int height)
{
  if (ssprite->mask) {
    gdk_gc_set_clip_origin(civ_gc, pixmap_x, pixmap_y);
    gdk_gc_set_clip_mask(civ_gc, ssprite->mask);
  }

  gdk_draw_pixmap(pixmap, civ_gc, ssprite->pixmap,
		  offset_x, offset_y,
		  pixmap_x + offset_x, pixmap_y + offset_y,
		  MIN(width, MAX(0, ssprite->width - offset_x)),
		  MIN(height, MAX(0, ssprite->height - offset_y)));

  gdk_gc_set_clip_mask(civ_gc, NULL);
}

/**************************************************************************
  Draw some or all of a sprite onto the mapview or citydialog canvas.
**************************************************************************/
void gui_put_sprite(struct canvas_store *pcanvas_store,
		    int canvas_x, int canvas_y,
		    struct Sprite *sprite,
		    int offset_x, int offset_y, int width, int height)
{
  pixmap_put_sprite(pcanvas_store->pixmap, canvas_x, canvas_y,
		    sprite, offset_x, offset_y, width, height);
}

/**************************************************************************
  Draw a full sprite onto the mapview or citydialog canvas.
**************************************************************************/
void gui_put_sprite_full(struct canvas_store *pcanvas_store,
			 int canvas_x, int canvas_y,
			 struct Sprite *sprite)
{
  gui_put_sprite(pcanvas_store, canvas_x, canvas_y,
		 sprite,
		 0, 0, sprite->width, sprite->height);
}

/**************************************************************************
  Place a (possibly masked) sprite on a pixmap.
**************************************************************************/
void pixmap_put_sprite_full(GdkDrawable *pixmap,
			    int pixmap_x, int pixmap_y,
			    struct Sprite *ssprite)
{
  pixmap_put_sprite(pixmap, pixmap_x, pixmap_y, ssprite,
		    0, 0, ssprite->width, ssprite->height);
}

/**************************************************************************
  Draw a filled-in colored rectangle onto the mapview or citydialog canvas.
**************************************************************************/
void gui_put_rectangle(struct canvas_store *pcanvas_store,
		       enum color_std color,
		       int canvas_x, int canvas_y, int width, int height)
{
  gdk_gc_set_foreground(fill_bg_gc, colors_standard[color]);
  gdk_draw_rectangle(pcanvas_store->pixmap, fill_bg_gc, TRUE,
		     canvas_x, canvas_y, width, height);
}

/**************************************************************************
  Draw a 1-pixel-width colored line onto the mapview or citydialog canvas.
**************************************************************************/
void gui_put_line(struct canvas_store *pcanvas_store, enum color_std color,
		  int start_x, int start_y, int dx, int dy)
{
  gdk_gc_set_foreground(civ_gc, colors_standard[color]);
  gdk_draw_line(pcanvas_store->pixmap, civ_gc,
		start_x, start_y, start_x + dx, start_y + dy);
}

/**************************************************************************
Only used for isometric view.
**************************************************************************/
static void pixmap_put_overlay_tile_draw(GdkDrawable *pixmap,
					 int canvas_x, int canvas_y,
					 struct Sprite *ssprite,
					 int offset_x, int offset_y,
					 int width, int height,
					 int fog)
{
  if (!ssprite || !width || !height)
    return;

  pixmap_put_sprite(pixmap, canvas_x, canvas_y, ssprite,
		    offset_x, offset_y, width, height);

  /* I imagine this could be done more efficiently. Some pixels We first
     draw from the sprite, and then draw black afterwards. It would be much
     faster to just draw every second pixel black in the first place. */
  if (fog) {
    gdk_gc_set_clip_origin(fill_tile_gc, canvas_x, canvas_y);
    gdk_gc_set_clip_mask(fill_tile_gc, ssprite->mask);
    gdk_gc_set_foreground(fill_tile_gc, colors_standard[COLOR_STD_BLACK]);
    gdk_gc_set_stipple(fill_tile_gc, black50);

    gdk_draw_rectangle(pixmap, fill_tile_gc, TRUE,
		       canvas_x+offset_x, canvas_y+offset_y,
		       MIN(width, MAX(0, ssprite->width-offset_x)),
		       MIN(height, MAX(0, ssprite->height-offset_y)));
    gdk_gc_set_clip_mask(fill_tile_gc, NULL);
  }
}

/**************************************************************************
 Draws a cross-hair overlay on a tile
**************************************************************************/
void put_cross_overlay_tile(int x, int y)
{
  int canvas_x, canvas_y;
  get_canvas_xy(x, y, &canvas_x, &canvas_y);

  if (tile_visible_mapcanvas(x, y)) {
    pixmap_put_overlay_tile(map_canvas->window,
			    canvas_x, canvas_y,
			    sprites.user.attention);
  }
}

/**************************************************************************
...
**************************************************************************/
void put_city_workers(struct city *pcity, int color)
{
  int canvas_x, canvas_y;
  static struct city *last_pcity=NULL;

  if (color==-1) {
    if (pcity!=last_pcity)
      city_workers_color = city_workers_color%3 + 1;
    color=city_workers_color;
  }
  gdk_gc_set_foreground(fill_tile_gc, colors_standard[color]);

  city_map_checked_iterate(pcity->x, pcity->y, i, j, x, y) {
    enum city_tile_type worked = get_worker_city(pcity, i, j);

    get_canvas_xy(x, y, &canvas_x, &canvas_y);

    /* stipple the area */
    if (!is_city_center(i, j)) {
      if (worked == C_TILE_EMPTY) {
	gdk_gc_set_stipple(fill_tile_gc, gray25);
      } else if (worked == C_TILE_WORKER) {
	gdk_gc_set_stipple(fill_tile_gc, gray50);
      } else
	continue;

      if (is_isometric) {
	gdk_gc_set_clip_origin(fill_tile_gc, canvas_x, canvas_y);
	gdk_gc_set_clip_mask(fill_tile_gc, sprites.black_tile->mask);
	gdk_draw_pixmap(map_canvas->window, fill_tile_gc, map_canvas_store,
			canvas_x, canvas_y,
			canvas_x, canvas_y,
			NORMAL_TILE_WIDTH, NORMAL_TILE_HEIGHT);
	gdk_draw_rectangle(map_canvas->window, fill_tile_gc, TRUE,
			   canvas_x, canvas_y,
			   NORMAL_TILE_WIDTH, NORMAL_TILE_HEIGHT);
	gdk_gc_set_clip_mask(fill_tile_gc, NULL);
      } else {
	gdk_draw_pixmap(map_canvas->window, civ_gc, map_canvas_store,
			canvas_x, canvas_y,
			canvas_x, canvas_y,
			NORMAL_TILE_WIDTH, NORMAL_TILE_HEIGHT);
	gdk_draw_rectangle(map_canvas->window, fill_tile_gc, TRUE,
			   canvas_x, canvas_y,
			   NORMAL_TILE_WIDTH, NORMAL_TILE_HEIGHT);
      }
    }

    /* draw tile output */
    if (worked == C_TILE_WORKER) {
      put_city_tile_output(map_canvas->window,
			   canvas_x, canvas_y,
			   city_get_food_tile(i, j, pcity),
			   city_get_shields_tile(i, j, pcity),
			   city_get_trade_tile(i, j, pcity));
    }
  } city_map_checked_iterate_end;

  last_pcity=pcity;
}

/**************************************************************************
...
**************************************************************************/
void update_map_canvas_scrollbars(void)
{
  gtk_adjustment_set_value(GTK_ADJUSTMENT(map_hadj), map_view_x0);
  gtk_adjustment_set_value(GTK_ADJUSTMENT(map_vadj), map_view_y0);
}

/**************************************************************************
...
**************************************************************************/
void update_map_canvas_scrollbars_size(void)
{
  map_hadj=gtk_adjustment_new(-1, 0, map.xsize, 1,
	   map_canvas_store_twidth, map_canvas_store_twidth);
  map_vadj=gtk_adjustment_new(-1, 0, map.ysize+EXTRA_BOTTOM_ROW, 1,
	   map_canvas_store_theight, map_canvas_store_theight);
  gtk_range_set_adjustment(GTK_RANGE(map_horizontal_scrollbar),
	GTK_ADJUSTMENT(map_hadj));
  gtk_range_set_adjustment(GTK_RANGE(map_vertical_scrollbar),
	GTK_ADJUSTMENT(map_vadj));

  gtk_signal_connect(GTK_OBJECT(map_hadj), "value_changed",
		     GTK_SIGNAL_FUNC(scrollbar_jump_callback),
		     GINT_TO_POINTER(TRUE));
  gtk_signal_connect(GTK_OBJECT(map_vadj), "value_changed",
		     GTK_SIGNAL_FUNC(scrollbar_jump_callback),
		     GINT_TO_POINTER(FALSE));
}

/**************************************************************************
...
**************************************************************************/
void scrollbar_jump_callback(GtkAdjustment *adj, gpointer hscrollbar)
{
  int last_map_view_x0;
  int last_map_view_y0;

  gfloat percent=adj->value;

  if (!can_client_change_view()) {
    return;
  }

  last_map_view_x0=map_view_x0;
  last_map_view_y0=map_view_y0;

  if(hscrollbar)
    map_view_x0=percent;
  else {
    map_view_y0=percent;
    map_view_y0=(map_view_y0<0) ? 0 : map_view_y0;
    map_view_y0=
      (map_view_y0>map.ysize+EXTRA_BOTTOM_ROW-map_canvas_store_theight) ? 
      map.ysize+EXTRA_BOTTOM_ROW-map_canvas_store_theight :
      map_view_y0;
  }

  if (last_map_view_x0!=map_view_x0 || last_map_view_y0!=map_view_y0) {
    update_map_canvas_visible();
    refresh_overview_viewrect();
  }
}

  
/**************************************************************************
draw a line from src_x,src_y -> dest_x,dest_y on both map_canvas and
map_canvas_store
FIXME: We currently always draw the line.
Only used for isometric view.
**************************************************************************/
static void really_draw_segment(int src_x, int src_y, int dir,
				bool write_to_screen, bool force)
{
  int dest_x, dest_y, is_real;
  int canvas_start_x, canvas_start_y;
  int canvas_end_x, canvas_end_y;

  gdk_gc_set_foreground(thick_line_gc, colors_standard[COLOR_STD_CYAN]);

  is_real = MAPSTEP(dest_x, dest_y, src_x, src_y, dir);
  assert(is_real);

  /* Find middle of tiles. y-1 to not undraw the the middle pixel of a
     horizontal line when we refresh the tile below-between. */
  get_canvas_xy(src_x, src_y, &canvas_start_x, &canvas_start_y);
  get_canvas_xy(dest_x, dest_y, &canvas_end_x, &canvas_end_y);
  canvas_start_x += NORMAL_TILE_WIDTH/2;
  canvas_start_y += NORMAL_TILE_HEIGHT/2-1;
  canvas_end_x += NORMAL_TILE_WIDTH/2;
  canvas_end_y += NORMAL_TILE_HEIGHT/2-1;

  /* somewhat hackish way of solving the problem where draw from a tile on
     one side of the screen out of the screen, and the tile we draw to is
     found to be on the other side of the screen. */
  if (abs(canvas_end_x - canvas_start_x) > NORMAL_TILE_WIDTH
      || abs(canvas_end_y - canvas_start_y) > NORMAL_TILE_HEIGHT)
    return;

  /* draw it! */
  gdk_draw_line(map_canvas_store, thick_line_gc,
		canvas_start_x, canvas_start_y, canvas_end_x, canvas_end_y);
  if (write_to_screen)
    gdk_draw_line(map_canvas->window, thick_line_gc,
		  canvas_start_x, canvas_start_y, canvas_end_x, canvas_end_y);
  return;
}

/**************************************************************************
...
**************************************************************************/
void draw_segment(int src_x, int src_y, int dir)
{
  assert(get_drawn(src_x, src_y, dir) > 0);

  if (is_isometric) {
    really_draw_segment(src_x, src_y, dir, TRUE, FALSE);
  } else {
    int dest_x, dest_y, is_real;

    is_real = MAPSTEP(dest_x, dest_y, src_x, src_y, dir);
    assert(is_real);

    if (tile_visible_mapcanvas(src_x, src_y)) {
      put_line(map_canvas_store, src_x, src_y, dir);
      put_line(map_canvas->window, src_x, src_y, dir);
    }
    if (tile_visible_mapcanvas(dest_x, dest_y)) {
      put_line(map_canvas_store, dest_x, dest_y, DIR_REVERSE(dir));
      put_line(map_canvas->window, dest_x, dest_y, DIR_REVERSE(dir));
    }
  }
}

/**************************************************************************
Not used in isometric view.
**************************************************************************/
static void put_line(GdkDrawable *pm, int x, int y, int dir)
{
  int canvas_src_x, canvas_src_y, canvas_dest_x, canvas_dest_y;
  get_canvas_xy(x, y, &canvas_src_x, &canvas_src_y);
  canvas_src_x += NORMAL_TILE_WIDTH/2;
  canvas_src_y += NORMAL_TILE_HEIGHT/2;
  DIRSTEP(canvas_dest_x, canvas_dest_y, dir);
  canvas_dest_x = canvas_src_x + (NORMAL_TILE_WIDTH * canvas_dest_x) / 2;
  canvas_dest_y = canvas_src_y + (NORMAL_TILE_WIDTH * canvas_dest_y) / 2;

  gdk_gc_set_foreground(civ_gc, colors_standard[COLOR_STD_CYAN]);

  gdk_draw_line(pm, civ_gc,
		canvas_src_x, canvas_src_y,
		canvas_dest_x, canvas_dest_y);
}

/**************************************************************************
Only used for isometric view.
**************************************************************************/
static void put_city_pixmap_draw(struct city *pcity, GdkPixmap *pm,
				 int canvas_x, int canvas_y,
				 int offset_x, int offset_y_unit,
				 int width, int height_unit,
				 int fog)
{
  struct Sprite *sprites[80];
  int count = fill_city_sprite_array_iso(sprites, pcity);
  int i;

  for (i=0; i<count; i++) {
    if (sprites[i]) {
      pixmap_put_overlay_tile_draw(pm, canvas_x, canvas_y, sprites[i],
				   offset_x, offset_y_unit,
				   width, height_unit,
				   fog);
    }
  }
}
/**************************************************************************
Only used for isometric view.
**************************************************************************/
static void pixmap_put_black_tile_iso(GdkDrawable *pm,
				      int canvas_x, int canvas_y,
				      int offset_x, int offset_y,
				      int width, int height)
{
  gdk_gc_set_clip_origin(civ_gc, canvas_x, canvas_y);
  gdk_gc_set_clip_mask(civ_gc, sprites.black_tile->mask);

  assert(width <= NORMAL_TILE_WIDTH);
  assert(height <= NORMAL_TILE_HEIGHT);
  gdk_draw_pixmap(pm, civ_gc, sprites.black_tile->pixmap,
		  offset_x, offset_y,
		  canvas_x+offset_x, canvas_y+offset_y,
		  width, height);

  gdk_gc_set_clip_mask(civ_gc, NULL);
}

/**************************************************************************
Blend the tile with neighboring tiles.
Only used for isometric view.
**************************************************************************/
static void dither_tile(GdkDrawable *pixmap, struct Sprite **dither,
			int canvas_x, int canvas_y,
			int offset_x, int offset_y,
			int width, int height, int fog)
{
  if (!width || !height)
    return;

  gdk_gc_set_clip_mask(civ_gc, sprites.dither_tile->mask);
  gdk_gc_set_clip_origin(civ_gc, canvas_x, canvas_y);
  assert(offset_x == 0 || offset_x == NORMAL_TILE_WIDTH/2);
  assert(offset_y == 0 || offset_y == NORMAL_TILE_HEIGHT/2);
  assert(width == NORMAL_TILE_WIDTH || width == NORMAL_TILE_WIDTH/2);
  assert(height == NORMAL_TILE_HEIGHT || height == NORMAL_TILE_HEIGHT/2);

  /* north */
  if (dither[0]
      && (offset_x != 0 || width == NORMAL_TILE_WIDTH)
      && (offset_y == 0)) {
    gdk_draw_pixmap(pixmap, civ_gc, dither[0]->pixmap,
		    NORMAL_TILE_WIDTH/2, 0,
		    canvas_x + NORMAL_TILE_WIDTH/2, canvas_y,
		    NORMAL_TILE_WIDTH/2, NORMAL_TILE_HEIGHT/2);
  }

  /* south */
  if (dither[1] && offset_x == 0
      && (offset_y == NORMAL_TILE_HEIGHT/2 || height == NORMAL_TILE_HEIGHT)) {
    gdk_draw_pixmap(pixmap, civ_gc, dither[1]->pixmap,
		    0, NORMAL_TILE_HEIGHT/2,
		    canvas_x,
		    canvas_y + NORMAL_TILE_HEIGHT/2,
		    NORMAL_TILE_WIDTH/2, NORMAL_TILE_HEIGHT/2);
  }

  /* east */
  if (dither[2]
      && (offset_x != 0 || width == NORMAL_TILE_WIDTH)
      && (offset_y != 0 || height == NORMAL_TILE_HEIGHT)) {
    gdk_draw_pixmap(pixmap, civ_gc, dither[2]->pixmap,
		    NORMAL_TILE_WIDTH/2, NORMAL_TILE_HEIGHT/2,
		    canvas_x + NORMAL_TILE_WIDTH/2,
		    canvas_y + NORMAL_TILE_HEIGHT/2,
		    NORMAL_TILE_WIDTH/2, NORMAL_TILE_HEIGHT/2);
  }

  /* west */
  if (dither[3] && offset_x == 0 && offset_y == 0) {
    gdk_draw_pixmap(pixmap, civ_gc, dither[3]->pixmap,
		    0, 0,
		    canvas_x,
		    canvas_y,
		    NORMAL_TILE_WIDTH/2, NORMAL_TILE_HEIGHT/2);
  }

  gdk_gc_set_clip_mask(civ_gc, NULL);

  if (fog) {
    gdk_gc_set_clip_origin(fill_tile_gc, canvas_x, canvas_y);
    gdk_gc_set_clip_mask(fill_tile_gc, sprites.dither_tile->mask);
    gdk_gc_set_foreground(fill_tile_gc, colors_standard[COLOR_STD_BLACK]);
    gdk_gc_set_stipple(fill_tile_gc, black50);

    gdk_draw_rectangle(pixmap, fill_tile_gc, TRUE,
		       canvas_x+offset_x, canvas_y+offset_y,
		       MIN(width, MAX(0, NORMAL_TILE_WIDTH-offset_x)),
		       MIN(height, MAX(0, NORMAL_TILE_HEIGHT-offset_y)));
    gdk_gc_set_clip_mask(fill_tile_gc, NULL);
  }
}

/**************************************************************************
Only used for isometric view.
**************************************************************************/
static void pixmap_put_tile_iso(GdkDrawable *pm, int x, int y,
				int canvas_x, int canvas_y,
				int citymode,
				int offset_x, int offset_y, int offset_y_unit,
				int width, int height, int height_unit,
				enum draw_type draw)
{
  struct Sprite *tile_sprs[80];
  struct Sprite *coasts[4];
  struct Sprite *dither[4];
  struct city *pcity;
  struct unit *punit, *pfocus;
  enum tile_special_type special;
  int count, i = 0;
  int fog;
  int solid_bg;

  if (!width || !(height || height_unit))
    return;

  count = fill_tile_sprite_array_iso(tile_sprs, coasts, dither,
				     x, y, citymode, &solid_bg);

  if (count == -1) { /* tile is unknown */
    pixmap_put_black_tile_iso(pm, canvas_x, canvas_y,
			      offset_x, offset_y, width, height);
    return;
  }

  /* Replace with check for is_normal_tile later */
  assert(is_real_map_pos(x, y));
  normalize_map_pos(&x, &y);

  fog = tile_get_known(x, y) == TILE_KNOWN_FOGGED && draw_fog_of_war;
  pcity = map_get_city(x, y);
  punit = get_drawable_unit(x, y, citymode);
  pfocus = get_unit_in_focus();
  special = map_get_special(x, y);

  if (solid_bg) {
    gdk_gc_set_clip_origin(fill_bg_gc, canvas_x, canvas_y);
    gdk_gc_set_clip_mask(fill_bg_gc, sprites.black_tile->mask);
    gdk_gc_set_foreground(fill_bg_gc, colors_standard[COLOR_STD_BACKGROUND]);

    gdk_draw_rectangle(pm, fill_bg_gc, TRUE,
		       canvas_x+offset_x, canvas_y+offset_y,
		       MIN(width, MAX(0, sprites.black_tile->width-offset_x)),
		       MIN(height, MAX(0, sprites.black_tile->height-offset_y)));
    gdk_gc_set_clip_mask(fill_bg_gc, NULL);
    if (fog) {
      gdk_gc_set_clip_origin(fill_tile_gc, canvas_x, canvas_y);
      gdk_gc_set_clip_mask(fill_tile_gc, sprites.black_tile->mask);
      gdk_gc_set_foreground(fill_tile_gc, colors_standard[COLOR_STD_BLACK]);
      gdk_gc_set_stipple(fill_tile_gc, black50);

      gdk_draw_rectangle(pm, fill_tile_gc, TRUE,
			 canvas_x+offset_x, canvas_y+offset_y,
			 MIN(width, MAX(0, sprites.black_tile->width-offset_x)),
			 MIN(height, MAX(0, sprites.black_tile->height-offset_y)));
      gdk_gc_set_clip_mask(fill_tile_gc, NULL);
    }
  }

  if (draw_terrain) {
    if (is_ocean(map_get_terrain(x, y))) { /* coasts */
      int dx, dy;
      /* top */
      dx = offset_x-NORMAL_TILE_WIDTH/4;
      pixmap_put_overlay_tile_draw(pm, canvas_x + NORMAL_TILE_WIDTH/4,
				   canvas_y, coasts[0],
				   MAX(0, dx),
				   offset_y,
				   MAX(0, width-MAX(0, -dx)),
				   height,
				   fog);
      /* bottom */
      dx = offset_x-NORMAL_TILE_WIDTH/4;
      dy = offset_y-NORMAL_TILE_HEIGHT/2;
      pixmap_put_overlay_tile_draw(pm, canvas_x + NORMAL_TILE_WIDTH/4,
				   canvas_y + NORMAL_TILE_HEIGHT/2, coasts[1],
				   MAX(0, dx),
				   MAX(0, dy),
				   MAX(0, width-MAX(0, -dx)),
				   MAX(0, height-MAX(0, -dy)),
				   fog);
      /* left */
      dy = offset_y-NORMAL_TILE_HEIGHT/4;
      pixmap_put_overlay_tile_draw(pm, canvas_x,
				   canvas_y + NORMAL_TILE_HEIGHT/4, coasts[2],
				   offset_x,
				   MAX(0, dy),
				   width,
				   MAX(0, height-MAX(0, -dy)),
				   fog);
      /* right */
      dx = offset_x-NORMAL_TILE_WIDTH/2;
      dy = offset_y-NORMAL_TILE_HEIGHT/4;
      pixmap_put_overlay_tile_draw(pm, canvas_x + NORMAL_TILE_WIDTH/2,
				   canvas_y + NORMAL_TILE_HEIGHT/4, coasts[3],
				   MAX(0, dx),
				   MAX(0, dy),
				   MAX(0, width-MAX(0, -dx)),
				   MAX(0, height-MAX(0, -dy)),
				   fog);
    } else {
      pixmap_put_overlay_tile_draw(pm, canvas_x, canvas_y, tile_sprs[0],
				   offset_x, offset_y, width, height, fog);
      i++;
    }

    /*** Dither base terrain ***/
    if (draw_terrain)
      dither_tile(pm, dither, canvas_x, canvas_y,
		  offset_x, offset_y, width, height, fog);
  }

  /*** Rest of terrain and specials ***/
  for (; i<count; i++) {
    if (tile_sprs[i])
      pixmap_put_overlay_tile_draw(pm, canvas_x, canvas_y, tile_sprs[i],
				   offset_x, offset_y, width, height, fog);
    else
      freelog(LOG_ERROR, "sprite is NULL");
  }

  /*** Map grid ***/
  if (draw_map_grid) {
    /* we draw the 2 lines on top of the tile; the buttom lines will be
       drawn by the tiles underneath. */
    if (draw & D_M_R) {
      gdk_gc_set_foreground(thin_line_gc,
			    colors_standard[get_grid_color
					    (x, y, x, y - 1)]);
      gdk_draw_line(pm, thin_line_gc,
		    canvas_x + NORMAL_TILE_WIDTH / 2, canvas_y,
		    canvas_x + NORMAL_TILE_WIDTH,
		    canvas_y + NORMAL_TILE_HEIGHT / 2);
    }

    if (draw & D_M_L) {
      gdk_gc_set_foreground(thin_line_gc,
			    colors_standard[get_grid_color
					    (x, y, x - 1, y)]);
      gdk_draw_line(pm, thin_line_gc,
		    canvas_x, canvas_y + NORMAL_TILE_HEIGHT / 2,
		    canvas_x + NORMAL_TILE_WIDTH / 2, canvas_y);
    }
  }

  if (draw_coastline && !draw_terrain) {
    enum tile_terrain_type t1 = map_get_terrain(x, y), t2;
    int x1, y1;
    gdk_gc_set_foreground(thin_line_gc, colors_standard[COLOR_STD_OCEAN]);
    x1 = x; y1 = y-1;
    if (normalize_map_pos(&x1, &y1)) {
      t2 = map_get_terrain(x1, y1);
      if (draw & D_M_R && (is_ocean(t1) ^ is_ocean(t2))) {
	gdk_draw_line(pm, thin_line_gc,
		      canvas_x+NORMAL_TILE_WIDTH/2, canvas_y,
		      canvas_x+NORMAL_TILE_WIDTH, canvas_y+NORMAL_TILE_HEIGHT/2);
      }
    }
    x1 = x-1; y1 = y;
    if (normalize_map_pos(&x1, &y1)) {
      t2 = map_get_terrain(x1, y1);
      if (draw & D_M_L && (is_ocean(t1) ^ is_ocean(t2))) {
	gdk_draw_line(pm, thin_line_gc,
		      canvas_x, canvas_y + NORMAL_TILE_HEIGHT/2,
		      canvas_x+NORMAL_TILE_WIDTH/2, canvas_y);
      }
    }
  }

  /*** City and various terrain improvements ***/
  if (pcity && draw_cities) {
    put_city_pixmap_draw(pcity, pm,
			 canvas_x, canvas_y - NORMAL_TILE_HEIGHT/2,
			 offset_x, offset_y_unit,
			 width, height_unit, fog);
  }
  if (contains_special(special, S_AIRBASE) && draw_fortress_airbase)
    pixmap_put_overlay_tile_draw(pm,
				 canvas_x, canvas_y-NORMAL_TILE_HEIGHT/2,
				 sprites.tx.airbase,
				 offset_x, offset_y_unit,
				 width, height_unit, fog);
  if (contains_special(special, S_FALLOUT) && draw_pollution)
    pixmap_put_overlay_tile_draw(pm,
				 canvas_x, canvas_y,
				 sprites.tx.fallout,
				 offset_x, offset_y,
				 width, height, fog);
  if (contains_special(special, S_POLLUTION) && draw_pollution)
    pixmap_put_overlay_tile_draw(pm,
				 canvas_x, canvas_y,
				 sprites.tx.pollution,
				 offset_x, offset_y,
				 width, height, fog);

  /*** city size ***/
  /* Not fogged as it would be unreadable */
  if (pcity && draw_cities) {
    if (pcity->size>=10)
      pixmap_put_overlay_tile_draw(pm, canvas_x, canvas_y-NORMAL_TILE_HEIGHT/2,
				   sprites.city.size_tens[pcity->size/10],
				   offset_x, offset_y_unit,
				   width, height_unit, 0);

    pixmap_put_overlay_tile_draw(pm, canvas_x, canvas_y-NORMAL_TILE_HEIGHT/2,
				 sprites.city.size[pcity->size%10],
				 offset_x, offset_y_unit,
				 width, height_unit, 0);
  }

  /*** Unit ***/
  if (punit && (draw_units || (punit == pfocus && draw_focus_unit))) {
    put_unit_pixmap_draw(punit, pm,
			 canvas_x, canvas_y - NORMAL_TILE_HEIGHT/2,
			 offset_x, offset_y_unit,
			 width, height_unit);
    if (!pcity && unit_list_size(&map_get_tile(x, y)->units) > 1)
      pixmap_put_overlay_tile_draw(pm,
				   canvas_x, canvas_y-NORMAL_TILE_HEIGHT/2,
				   sprites.unit.stack,
				   offset_x, offset_y_unit,
				   width, height_unit, fog);
  }

  if (contains_special(special, S_FORTRESS) && draw_fortress_airbase)
    pixmap_put_overlay_tile_draw(pm,
				 canvas_x, canvas_y-NORMAL_TILE_HEIGHT/2,
				 sprites.tx.fortress,
				 offset_x, offset_y_unit,
				 width, height_unit, fog);
}

/**************************************************************************
  This function is called when the tileset is changed.
**************************************************************************/
void tileset_changed(void)
{
  reset_city_dialogs();
  reset_unit_table();

  /* single_tile is originally allocated in gui_main.c. */
  gdk_pixmap_unref(single_tile_pixmap);
  single_tile_pixmap = gdk_pixmap_new(root_window, 
				      UNIT_TILE_WIDTH, UNIT_TILE_HEIGHT, -1);
}
