// SPDX-License-Identifier: GPL-2.0-only
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <wayland-server-core.h>
#include <wlr/util/log.h>
#include "action.h"
#include "common/buf.h"
#include "common/dir.h"
#include "common/font.h"
#include "common/list.h"
#include "common/mem.h"
#include "common/nodename.h"
#include "common/scaled_font_buffer.h"
#include "common/scene-helpers.h"
#include "common/spawn.h"
#include "common/string-helpers.h"
#include "labwc.h"
#include "menu/menu.h"
#include "node.h"
#include "theme.h"

#define PIPEMENU_MAX_BUF_SIZE 1048576  /* 1 MiB */
#define PIPEMENU_TIMEOUT_IN_MS 4000    /* 4 seconds */

/* state-machine variables for processing <item></item> */
static bool in_item;
static struct menuitem *current_item;
static struct action *current_item_action;

static int menu_level;
static struct menu *current_menu;

static bool waiting_for_pipe_menu;
static struct menuitem *selected_item;

/* TODO: split this whole file into parser.c and actions.c*/

static bool
is_unique_id(struct server *server, const char *id)
{
	struct menu *menu;
	wl_list_for_each(menu, &server->menus, link) {
		if (!strcmp(menu->id, id)) {
			return false;
		}
	}
	return true;
}

static struct menu *
menu_create(struct server *server, const char *id, const char *label)
{
	if (!is_unique_id(server, id)) {
		wlr_log(WLR_ERROR, "menu id %s already exists", id);
	}

	struct menu *menu = znew(*menu);
	wl_list_append(&server->menus, &menu->link);

	wl_list_init(&menu->menuitems);
	menu->id = xstrdup(id);
	menu->label = xstrdup(label ? label : id);
	menu->parent = current_menu;
	menu->server = server;
	menu->is_pipemenu = waiting_for_pipe_menu;
	menu->size.width = server->theme->menu_min_width;
	/* menu->size.height will be kept up to date by adding items */
	menu->scene_tree = wlr_scene_tree_create(server->menu_tree);
	wlr_scene_node_set_enabled(&menu->scene_tree->node, false);
	return menu;
}

struct menu *
menu_get_by_id(struct server *server, const char *id)
{
	if (!id) {
		return NULL;
	}
	struct menu *menu;
	wl_list_for_each(menu, &server->menus, link) {
		if (!strcmp(menu->id, id)) {
			return menu;
		}
	}
	return NULL;
}

static void
menu_update_width(struct menu *menu)
{
	struct menuitem *item;
	struct theme *theme = menu->server->theme;
	int max_width = theme->menu_min_width;

	/* Get widest menu item, clamped by menu_max_width */
	wl_list_for_each(item, &menu->menuitems, link) {
		if (item->native_width > max_width) {
			max_width = item->native_width < theme->menu_max_width
				? item->native_width : theme->menu_max_width;
		}
	}
	menu->size.width = max_width + 2 * theme->menu_item_padding_x;

	/* Update all items for the new size */
	wl_list_for_each(item, &menu->menuitems, link) {
		wlr_scene_rect_set_size(
			wlr_scene_rect_from_node(item->normal.background),
			menu->size.width, item->height);

		if (!item->selected.background) {
			/* This is a separator. They don't have a selected background. */
			wlr_scene_rect_set_size(
				wlr_scene_rect_from_node(item->normal.text),
				menu->size.width - 2 * theme->menu_separator_padding_width,
				theme->menu_separator_line_thickness);
		} else {
			/* Usual menu item */
			wlr_scene_rect_set_size(
				wlr_scene_rect_from_node(item->selected.background),
				menu->size.width, item->height);
			if (item->native_width > max_width || item->submenu || item->execute) {
				scaled_font_buffer_set_max_width(item->normal.buffer,
					max_width);
				scaled_font_buffer_set_max_width(item->selected.buffer,
					max_width);
			}
		}
	}
}

static void
post_processing(struct server *server)
{
	struct menu *menu;
	wl_list_for_each(menu, &server->menus, link) {
		menu_update_width(menu);
	}
}

static void
validate_menu(struct menu *menu)
{
	struct menuitem *item;
	struct action *action, *action_tmp;
	wl_list_for_each(item, &menu->menuitems, link) {
		wl_list_for_each_safe(action, action_tmp, &item->actions, link) {
			if (!action_is_valid(action)) {
				wl_list_remove(&action->link);
				action_free(action);
				wlr_log(WLR_ERROR, "Removed invalid menu action");
			}
		}
	}
}

static void
validate(struct server *server)
{
	struct menu *menu;
	wl_list_for_each(menu, &server->menus, link) {
		validate_menu(menu);
	}
}

static struct menuitem *
item_create(struct menu *menu, const char *text, bool show_arrow)
{
	assert(menu);
	assert(text);

	struct menuitem *menuitem = znew(*menuitem);
	menuitem->parent = menu;
	menuitem->selectable = true;
	struct server *server = menu->server;
	struct theme *theme = server->theme;

	const char *arrow = show_arrow ? "›" : NULL;

	if (!menu->item_height) {
		menu->item_height = font_height(&rc.font_menuitem)
			+ 2 * theme->menu_item_padding_y;
	}
	menuitem->height = menu->item_height;

	int x, y;
	menuitem->native_width = font_width(&rc.font_menuitem, text);
	if (arrow) {
		menuitem->native_width += font_width(&rc.font_menuitem, arrow);
	}

	/* Menu item root node */
	menuitem->tree = wlr_scene_tree_create(menu->scene_tree);
	node_descriptor_create(&menuitem->tree->node,
		LAB_NODE_DESC_MENUITEM, menuitem);

	/* Tree for each state to hold background and text buffer */
	menuitem->normal.tree = wlr_scene_tree_create(menuitem->tree);
	menuitem->selected.tree = wlr_scene_tree_create(menuitem->tree);

	/* Item background nodes */
	menuitem->normal.background = &wlr_scene_rect_create(
		menuitem->normal.tree,
		menu->size.width, menu->item_height,
		theme->menu_items_bg_color)->node;
	menuitem->selected.background = &wlr_scene_rect_create(
		menuitem->selected.tree,
		menu->size.width, menu->item_height,
		theme->menu_items_active_bg_color)->node;

	/* Font nodes */
	menuitem->normal.buffer = scaled_font_buffer_create(menuitem->normal.tree);
	menuitem->selected.buffer = scaled_font_buffer_create(menuitem->selected.tree);
	if (!menuitem->normal.buffer || !menuitem->selected.buffer) {
		wlr_log(WLR_ERROR, "Failed to create menu item '%s'", text);
		/*
		 * Destroying the root node will destroy everything,
		 * including the node descriptor and scaled_font_buffers.
		 */
		wlr_scene_node_destroy(&menuitem->tree->node);
		free(menuitem);
		return NULL;
	}
	menuitem->normal.text = &menuitem->normal.buffer->scene_buffer->node;
	menuitem->selected.text = &menuitem->selected.buffer->scene_buffer->node;

	/* Font buffers */
	scaled_font_buffer_update(menuitem->normal.buffer, text, menuitem->native_width,
		&rc.font_menuitem, theme->menu_items_text_color,
		theme->menu_items_bg_color, arrow);
	scaled_font_buffer_update(menuitem->selected.buffer, text, menuitem->native_width,
		&rc.font_menuitem, theme->menu_items_active_text_color,
		theme->menu_items_active_bg_color, arrow);

	/* Center font nodes */
	x = theme->menu_item_padding_x;
	y = (menu->item_height - menuitem->normal.buffer->height) / 2;
	wlr_scene_node_set_position(menuitem->normal.text, x, y);
	y = (menu->item_height - menuitem->selected.buffer->height) / 2;
	wlr_scene_node_set_position(menuitem->selected.text, x, y);

	/* Position the item in relation to its menu */
	wlr_scene_node_set_position(&menuitem->tree->node, 0, menu->size.height);

	/* Hide selected state */
	wlr_scene_node_set_enabled(&menuitem->selected.tree->node, false);

	/* Update menu extents */
	menu->size.height += menuitem->height;

	wl_list_append(&menu->menuitems, &menuitem->link);
	wl_list_init(&menuitem->actions);
	return menuitem;
}

static struct menuitem *
separator_create(struct menu *menu, const char *label)
{
	struct menuitem *menuitem = znew(*menuitem);
	menuitem->parent = menu;
	menuitem->selectable = false;
	struct server *server = menu->server;
	struct theme *theme = server->theme;
	menuitem->height = theme->menu_separator_line_thickness +
			2 * theme->menu_separator_padding_height;

	menuitem->tree = wlr_scene_tree_create(menu->scene_tree);
	node_descriptor_create(&menuitem->tree->node,
		LAB_NODE_DESC_MENUITEM, menuitem);
	menuitem->normal.tree = wlr_scene_tree_create(menuitem->tree);
	menuitem->normal.background = &wlr_scene_rect_create(
		menuitem->normal.tree,
		menu->size.width, menuitem->height,
		theme->menu_items_bg_color)->node;

	int width = menu->size.width - 2 * theme->menu_separator_padding_width;
	menuitem->normal.text = &wlr_scene_rect_create(
		menuitem->normal.tree,
		width > 0 ? width : 0,
		theme->menu_separator_line_thickness,
		theme->menu_separator_color)->node;

	wlr_scene_node_set_position(&menuitem->tree->node, 0, menu->size.height);

	/* Vertically center-align separator line */
	wlr_scene_node_set_position(menuitem->normal.text,
		theme->menu_separator_padding_width,
		theme->menu_separator_padding_height);

	menu->size.height += menuitem->height;
	wl_list_append(&menu->menuitems, &menuitem->link);
	wl_list_init(&menuitem->actions);
	return menuitem;
}

/*
 * Handle the following:
 * <item label="">
 *   <action name="">
 *     <command></command>
 *   </action>
 * </item>
 */
static void
fill_item(char *nodename, char *content)
{
	/*
	 * Nodenames for most menu-items end with '.item.menu' but top-level
	 * pipemenu items do not have the associated <menu> element so merely
	 * end with a '.item'
	 */
	string_truncate_at_pattern(nodename, ".item.menu");
	string_truncate_at_pattern(nodename, ".item");

	/* <item label=""> defines the start of a new item */
	if (!strcmp(nodename, "label")) {
		current_item = item_create(current_menu, content, false);
		current_item_action = NULL;
	} else if (!current_item) {
		wlr_log(WLR_ERROR, "expect <item label=\"\"> element first. "
			"nodename: '%s' content: '%s'", nodename, content);
	} else if (!strcmp(nodename, "icon")) {
		/*
		 * Do nothing as we don't support menu icons - just avoid
		 * logging errors if a menu.xml file contains icon="" entries.
		 */
	} else if (!strcmp(nodename, "name.action")) {
		current_item_action = action_create(content);
		if (current_item_action) {
			wl_list_append(&current_item->actions,
				&current_item_action->link);
		}
	} else if (!current_item_action) {
		wlr_log(WLR_ERROR, "expect <action name=\"\"> element first. "
			"nodename: '%s' content: '%s'", nodename, content);
	} else {
		action_arg_from_xml_node(current_item_action, nodename, content);
	}
}

static void
item_destroy(struct menuitem *item)
{
	wl_list_remove(&item->link);
	action_list_free(&item->actions);
	wlr_scene_node_destroy(&item->tree->node);
	free(item->execute);
	free(item->id);
	free(item);
}

/*
 * We support XML CDATA for <command> in menu.xml in order to provide backward
 * compatibility with obmenu-generator. For example:
 *
 * <menu id="" label="">
 *   <item label="">
 *     <action name="Execute">
 *       <command><![CDATA[xdg-open .]]></command>
 *     </action>
 *   </item>
 * </menu>
 *
 * <execute> is an old, deprecated openbox variety of <command>. We support it
 * for backward compatibility with old openbox-menu generators. It has the same
 * function and <command>
 *
 * The following nodenames support CDATA.
 *  - command.action.item.*menu.openbox_menu
 *  - execute.action.item.*menu.openbox_menu
 *  - command.action.item.openbox_pipe_menu
 *  - execute.action.item.openbox_pipe_menu
 *  - command.action.item.*menu.openbox_pipe_menu
 *  - execute.action.item.*menu.openbox_pipe_menu
 *
 * The *menu allows nested menus with nodenames such as ...menu.menu... or
 * ...menu.menu.menu... and so on. We could use match_glob() for all of the
 * above but it seems simpler to just check the first three fields.
 */
static bool
nodename_supports_cdata(char *nodename)
{
	return !strncmp("command.action.", nodename, 15)
		|| !strncmp("execute.action.", nodename, 15);
}

static void
entry(xmlNode *node, char *nodename, char *content)
{
	if (!nodename) {
		return;
	}
	xmlChar *cdata = NULL;
	if (!content && nodename_supports_cdata(nodename)) {
		cdata = xmlNodeGetContent(node);
	}
	if (!content && !cdata) {
		return;
	}
	string_truncate_at_pattern(nodename, ".openbox_menu");
	string_truncate_at_pattern(nodename, ".openbox_pipe_menu");
	if (getenv("LABWC_DEBUG_MENU_NODENAMES")) {
		printf("%s: %s\n", nodename, content ? content : (char *)cdata);
	}
	if (in_item) {
		fill_item(nodename, content ? content : (char *)cdata);
	}
	xmlFree(cdata);
}

static void
process_node(xmlNode *node)
{
	static char buffer[256];

	char *content = (char *)node->content;
	if (xmlIsBlankNode(node)) {
		return;
	}
	char *name = nodename(node, buffer, sizeof(buffer));
	entry(node, name, content);
}

static void xml_tree_walk(xmlNode *node, struct server *server);

static void
traverse(xmlNode *n, struct server *server)
{
	xmlAttr *attr;

	process_node(n);
	for (attr = n->properties; attr; attr = attr->next) {
		xml_tree_walk(attr->children, server);
	}
	xml_tree_walk(n->children, server);
}

static int
nr_parents(xmlNode *n)
{
	assert(n);
	int i = 0;
	for (xmlNode *node = n->parent; node && i < INT_MAX; ++i) {
		node = node->parent;
	}
	return i;
}

/*
 * Return true for the highest level static menu definitions in the format
 * below. We use the fact that the id-attribute has two nodal parents (<menu>
 * and <openbox_menu>) as the test here.
 *
 *     <openbox_menu>
 *       <menu id="">
 *          ...
 *       </menu>
 *     </openbox_menu>
 *
 * Return false for any other <menu id=""> element which could be either:
 *
 *   (a) one found in a pipemenu; or
 *   (b) one that links to a submenu as follows (but is a child to another
 *       <menu> element.
 *
 *     <menu id="root-menu">
 *       <!-- this links to a sub menu -->
 *       <menu id="submenu-defined-elsewhere"/>
 *     </menu>
 */
static bool
is_toplevel_static_menu_definition(xmlNode *n, char *id)
{
	/*
	 * Catch <menu id=""> elements in pipemenus
	 *
	 * For pipemenus we cannot just rely on nr_parents() because they have
	 * their own hierarchy, so we just use the fact that a pipemenu cannot
	 * be the root-menu.
	 */
	if (menu_level) {
		return false;
	}

	return id && nr_parents(n) == 2;
}

/*
 * <menu> elements have three different roles:
 *  * Definition of (sub)menu - has ID, LABEL and CONTENT
 *  * Menuitem of pipemenu type - has ID, LABEL and EXECUTE
 *  * Menuitem of submenu type - has ID only
 */
static void
handle_menu_element(xmlNode *n, struct server *server)
{
	char *label = (char *)xmlGetProp(n, (const xmlChar *)"label");
	char *execute = (char *)xmlGetProp(n, (const xmlChar *)"execute");
	char *id = (char *)xmlGetProp(n, (const xmlChar *)"id");

	if (execute && label && id) {
		wlr_log(WLR_DEBUG, "pipemenu '%s:%s:%s'", id, label, execute);
		current_item = item_create(current_menu, label, /* arrow */ true);
		current_item_action = NULL;
		current_item->execute = xstrdup(execute);
		current_item->id = xstrdup(id);
	} else if ((label && id) || is_toplevel_static_menu_definition(n, id)) {
		/*
		 * (label && id) refers to <menu id="" label=""> which is an
		 * inline menu definition.
		 *
		 * is_toplevel_static_menu_definition() catches:
		 *     <openbox_menu>
		 *       <menu id=""></menu>
		 *     </openbox_menu>
		 *
		 * which is the highest level a menu can be defined at.
		 *
		 * Openbox spec requires a label="" defined here, but it is
		 * actually pointless so we handle it with or without the label
		 * attribute to make it easier for users to define "root-menu"
		 * and "client-menu".
		 */
		struct menu **submenu = NULL;
		if (menu_level > 0) {
			/*
			 * In a nested (inline) menu definition we need to
			 * create an item pointing to the new submenu
			 */
			current_item = item_create(current_menu, label, true);
			if (current_item) {
				submenu = &current_item->submenu;
			} else {
				submenu = NULL;
			}
		}
		++menu_level;
		current_menu = menu_create(server, id, label);
		if (submenu) {
			*submenu = current_menu;
		}
		traverse(n, server);
		current_menu = current_menu->parent;
		--menu_level;
	} else if (id) {
		/*
		 * <menu id=""> (when inside another <menu> element) creates an
		 * entry which points to a menu defined elsewhere.
		 *
		 * This is only supported in static menus. Pipemenus need to use
		 * nested (inline) menu definitions, otherwise we could have a
		 * pipemenu opening the "root-menu" or similar.
		 */

		if (current_menu && current_menu->is_pipemenu) {
			wlr_log(WLR_ERROR,
				"cannot link to static menu from pipemenu");
			goto error;
		}

		struct menu *menu = menu_get_by_id(server, id);
		if (menu) {
			current_item = item_create(current_menu, menu->label, true);
			if (current_item) {
				current_item->submenu = menu;
			}
		} else {
			wlr_log(WLR_ERROR, "no menu with id '%s'", id);
		}
	}
error:
	free(label);
	free(execute);
	free(id);
}

/* This can be one of <separator> and <separator label=""> */
static void
handle_separator_element(xmlNode *n)
{
	char *label = (char *)xmlGetProp(n, (const xmlChar *)"label");
	current_item = separator_create(current_menu, label);
	free(label);
}

static void
xml_tree_walk(xmlNode *node, struct server *server)
{
	for (xmlNode *n = node; n && n->name; n = n->next) {
		if (!strcasecmp((char *)n->name, "comment")) {
			continue;
		}
		if (!strcasecmp((char *)n->name, "menu")) {
			handle_menu_element(n, server);
			continue;
		}
		if (!strcasecmp((char *)n->name, "separator")) {
			handle_separator_element(n);
			continue;
		}
		if (!strcasecmp((char *)n->name, "item")) {
			in_item = true;
			traverse(n, server);
			in_item = false;
			continue;
		}
		traverse(n, server);
	}
}

static bool
parse_buf(struct server *server, struct buf *buf)
{
	xmlDoc *d = xmlParseMemory(buf->data, buf->len);
	if (!d) {
		wlr_log(WLR_ERROR, "xmlParseMemory()");
		return false;
	}
	xml_tree_walk(xmlDocGetRootElement(d), server);
	xmlFreeDoc(d);
	xmlCleanupParser();
	return true;
}

/*
 * @stream can come from either of the following:
 *   - fopen() in the case of reading a file such as menu.xml
 *   - popen() when processing pipemenus
 */
static void
parse_stream(struct server *server, FILE *stream)
{
	char *line = NULL;
	size_t len = 0;
	struct buf b = BUF_INIT;

	while (getline(&line, &len, stream) != -1) {
		char *p = strrchr(line, '\n');
		if (p) {
			*p = '\0';
		}
		buf_add(&b, line);
	}
	free(line);
	parse_buf(server, &b);
	buf_reset(&b);
}

static void
parse_xml(const char *filename, struct server *server)
{
	struct wl_list paths;
	paths_config_create(&paths, filename);

	bool should_merge_config = rc.merge_config;
	struct wl_list *(*iter)(struct wl_list *list);
	iter = should_merge_config ? paths_get_prev : paths_get_next;

	for (struct wl_list *elm = iter(&paths); elm != &paths; elm = iter(elm)) {
		struct path *path = wl_container_of(elm, path, link);
		FILE *stream = fopen(path->string, "r");
		if (!stream) {
			return;
		}
		wlr_log(WLR_INFO, "read menu file %s", path->string);
		parse_stream(server, stream);
		fclose(stream);
		if (!should_merge_config) {
			break;
		}
	}
	paths_destroy(&paths);
}

static int
menu_get_full_width(struct menu *menu)
{
	int width = menu->size.width - menu->server->theme->menu_overlap_x;
	int child_width;
	int max_child_width = 0;
	struct menuitem *item;
	wl_list_for_each(item, &menu->menuitems, link) {
		if (!item->submenu) {
			continue;
		}
		child_width = menu_get_full_width(item->submenu);
		if (child_width > max_child_width) {
			max_child_width = child_width;
		}
	}
	return width + max_child_width;
}

static struct wlr_box
get_submenu_position(struct menuitem *item, enum menu_align align)
{
	struct wlr_box pos = { 0 };
	struct menu *menu = item->parent;
	struct theme *theme = menu->server->theme;
	int lx = menu->scene_tree->node.x;
	int ly = menu->scene_tree->node.y;

	if (align & LAB_MENU_OPEN_RIGHT) {
		pos.x = lx + menu->size.width - theme->menu_overlap_x;
	} else {
		pos.x = lx;
	}
	int rel_y = item->tree->node.y;
	pos.y = ly + rel_y - theme->menu_overlap_y;
	return pos;
}

static void
menu_configure(struct menu *menu, int lx, int ly, enum menu_align align)
{
	struct theme *theme = menu->server->theme;

	/* Get output local coordinates + output usable area */
	double ox = lx;
	double oy = ly;
	struct wlr_output *wlr_output = wlr_output_layout_output_at(
		menu->server->output_layout, lx, ly);
	struct output *output = wlr_output ? output_from_wlr_output(
		menu->server, wlr_output) : NULL;
	if (!output) {
		wlr_log(WLR_ERROR,
			"Failed to position menu %s (%s) and its submenus: "
			"Not enough screen space", menu->id, menu->label);
		return;
	}
	wlr_output_layout_output_coords(menu->server->output_layout,
		wlr_output, &ox, &oy);

	if (align == LAB_MENU_OPEN_AUTO) {
		int full_width = menu_get_full_width(menu);
		if (ox + full_width > output->usable_area.width) {
			align = LAB_MENU_OPEN_LEFT;
		} else {
			align = LAB_MENU_OPEN_RIGHT;
		}
	}

	if (oy + menu->size.height > output->usable_area.height) {
		align &= ~LAB_MENU_OPEN_BOTTOM;
		align |= LAB_MENU_OPEN_TOP;
	} else {
		align &= ~LAB_MENU_OPEN_TOP;
		align |= LAB_MENU_OPEN_BOTTOM;
	}

	if (align & LAB_MENU_OPEN_LEFT) {
		lx -= menu->size.width - theme->menu_overlap_x;
	}
	if (align & LAB_MENU_OPEN_TOP) {
		ly -= menu->size.height;
		if (menu->parent) {
			/* For submenus adjust y to bottom left corner */
			ly += menu->item_height;
		}
	}
	wlr_scene_node_set_position(&menu->scene_tree->node, lx, ly);

	/* Needed for pipemenus to inherit alignment */
	menu->align = align;

	struct menuitem *item;
	wl_list_for_each(item, &menu->menuitems, link) {
		if (!item->submenu) {
			continue;
		}
		struct wlr_box pos = get_submenu_position(item, align);
		menu_configure(item->submenu, pos.x, pos.y, align);
	}
}

static void
menu_hide_submenu(struct server *server, const char *id)
{
	struct menu *menu, *hide_menu;
	hide_menu = menu_get_by_id(server, id);
	if (!hide_menu) {
		return;
	}
	wl_list_for_each(menu, &server->menus, link) {
		bool should_reposition = false;
		struct menuitem *item, *next;
		wl_list_for_each_safe(item, next, &menu->menuitems, link) {
			if (item->submenu == hide_menu) {
				item_destroy(item);
				should_reposition = true;
			}
		}

		if (!should_reposition) {
			continue;
		}
		/* Re-position items vertically */
		menu->size.height = 0;
		wl_list_for_each(item, &menu->menuitems, link) {
			wlr_scene_node_set_position(&item->tree->node, 0,
				menu->size.height);
			menu->size.height += item->height;
		}
	}
}

static void
init_rootmenu(struct server *server)
{
	struct menu *menu = menu_get_by_id(server, "root-menu");

	/* Default menu if no menu.xml found */
	if (!menu) {
		current_menu = NULL;
		menu = menu_create(server, "root-menu", "");
	}
	if (wl_list_empty(&menu->menuitems)) {
		current_item = item_create(menu, _("Reconfigure"), false);
		fill_item("name.action", "Reconfigure");
		current_item = item_create(menu, _("Exit"), false);
		fill_item("name.action", "Exit");
	}
}

static void
init_windowmenu(struct server *server)
{
	struct menu *menu = menu_get_by_id(server, "client-menu");

	/* Default menu if no menu.xml found */
	if (!menu) {
		current_menu = NULL;
		menu = menu_create(server, "client-menu", "");
	}
	if (wl_list_empty(&menu->menuitems)) {
		current_item = item_create(menu, _("Minimize"), false);
		fill_item("name.action", "Iconify");
		current_item = item_create(menu, _("Maximize"), false);
		fill_item("name.action", "ToggleMaximize");
		current_item = item_create(menu, _("Fullscreen"), false);
		fill_item("name.action", "ToggleFullscreen");
		current_item = item_create(menu, _("Roll up/down"), false);
		fill_item("name.action", "ToggleShade");
		current_item = item_create(menu, _("Decorations"), false);
		fill_item("name.action", "ToggleDecorations");
		current_item = item_create(menu, _("Always on Top"), false);
		fill_item("name.action", "ToggleAlwaysOnTop");

		/* Workspace sub-menu */
		struct menu *workspace_menu = menu_create(server, "workspaces", "");
		current_item = item_create(workspace_menu, _("Move left"), false);
		/*
		 * <action name="SendToDesktop"><follow> is true by default so
		 * GoToDesktop will be called as part of the action.
		 */
		fill_item("name.action", "SendToDesktop");
		fill_item("to.action", "left");
		current_item = item_create(workspace_menu, _("Move right"), false);
		fill_item("name.action", "SendToDesktop");
		fill_item("to.action", "right");
		current_item = separator_create(workspace_menu, "");
		current_item = item_create(workspace_menu,
			_("Always on Visible Workspace"), false);
		fill_item("name.action", "ToggleOmnipresent");

		current_item = item_create(menu, _("Workspace"), true);
		current_item->submenu = workspace_menu;

		current_item = item_create(menu, _("Close"), false);
		fill_item("name.action", "Close");
	}

	if (wl_list_length(&rc.workspace_config.workspaces) == 1) {
		menu_hide_submenu(server, "workspaces");
	}
}

void
menu_init(struct server *server)
{
	wl_list_init(&server->menus);
	parse_xml("menu.xml", server);
	init_rootmenu(server);
	init_windowmenu(server);
	post_processing(server);
	validate(server);
}

static void
nullify_item_pointing_to_this_menu(struct menu *menu)
{
	struct menu *iter;
	wl_list_for_each(iter, &menu->server->menus, link) {
		struct menuitem *item;
		wl_list_for_each(item, &iter->menuitems, link) {
			if (item->submenu == menu) {
				item->submenu = NULL;
				/*
				 * Let's not return early here in case we have
				 * multiple items pointing to the same menu.
				 */
			}
		}

		/* This is important for pipe-menus */
		if (iter->parent == menu) {
			iter->parent = NULL;
		}
	}
}

static void
menu_free(struct menu *menu)
{
	/* Keep items clean on pipemenu destruction */
	nullify_item_pointing_to_this_menu(menu);

	struct menuitem *item, *next;
	wl_list_for_each_safe(item, next, &menu->menuitems, link) {
		item_destroy(item);
	}

	/*
	 * Destroying the root node will destroy everything,
	 * including node descriptors and scaled_font_buffers.
	 */
	wlr_scene_node_destroy(&menu->scene_tree->node);
	wl_list_remove(&menu->link);
	zfree(menu);
}

/**
 * menu_free_from - free menu list starting from current point
 * @from: point to free from (if NULL, all menus are freed)
 */
static void
menu_free_from(struct server *server, struct menu *from)
{
	bool destroying = !from;
	struct menu *menu, *tmp_menu;
	wl_list_for_each_safe(menu, tmp_menu, &server->menus, link) {
		if (menu == from) {
			destroying = true;
		}
		if (!destroying) {
			continue;
		}

		menu_free(menu);
	}
}

void
menu_finish(struct server *server)
{
	menu_free_from(server, NULL);
}

/* Sets selection (or clears selection if passing NULL) */
static void
menu_set_selection(struct menu *menu, struct menuitem *item)
{
	/* Clear old selection */
	if (menu->selection.item) {
		wlr_scene_node_set_enabled(
			&menu->selection.item->normal.tree->node, true);
		wlr_scene_node_set_enabled(
			&menu->selection.item->selected.tree->node, false);
	}
	/* Set new selection */
	if (item) {
		wlr_scene_node_set_enabled(&item->normal.tree->node, false);
		wlr_scene_node_set_enabled(&item->selected.tree->node, true);
	}
	menu->selection.item = item;
}

static void
close_all_submenus(struct menu *menu)
{
	struct menuitem *item;
	wl_list_for_each(item, &menu->menuitems, link) {
		if (item->submenu) {
			wlr_scene_node_set_enabled(
				&item->submenu->scene_tree->node, false);
			close_all_submenus(item->submenu);
		}
	}
	menu->selection.menu = NULL;
}

/*
 * We only destroy pipemenus when closing the entire menu-tree so that pipemenu
 * are cached (for as long as the menu is open). This drastically improves the
 * felt performance when interacting with multiple pipe menus where a single
 * item may be selected multiple times.
 */
static void
destroy_pipemenus(struct server *server)
{
	wlr_log(WLR_DEBUG, "number of menus before close=%d",
		wl_list_length(&server->menus));

	struct menu *iter, *tmp;
	wl_list_for_each_safe(iter, tmp, &server->menus, link) {
		if (iter->is_pipemenu) {
			menu_free(iter);
		}
	}

	wlr_log(WLR_DEBUG, "number of menus after  close=%d",
		wl_list_length(&server->menus));
}

static void
_close(struct menu *menu)
{
	wlr_scene_node_set_enabled(&menu->scene_tree->node, false);
	menu_set_selection(menu, NULL);
	if (menu->selection.menu) {
		_close(menu->selection.menu);
		menu->selection.menu = NULL;
	}
}

static void
menu_close(struct menu *menu)
{
	if (!menu) {
		wlr_log(WLR_ERROR, "Trying to close non exiting menu");
		return;
	}
	_close(menu);
}

void
menu_open_root(struct menu *menu, int x, int y)
{
	assert(menu);
	if (menu->server->menu_current) {
		menu_close(menu->server->menu_current);
		destroy_pipemenus(menu->server);
	}
	close_all_submenus(menu);
	menu_set_selection(menu, NULL);
	menu_configure(menu, x, y, LAB_MENU_OPEN_AUTO);
	wlr_scene_node_set_enabled(&menu->scene_tree->node, true);
	menu->server->menu_current = menu;
	menu->server->input_mode = LAB_INPUT_STATE_MENU;
	selected_item = NULL;
}

struct pipe_context {
	struct server *server;
	struct menuitem *item;
	struct buf buf;
	struct wl_event_source *event_read;
	struct wl_event_source *event_timeout;
	pid_t pid;
	int pipe_fd;
};

static void
create_pipe_menu(struct pipe_context *ctx)
{
	struct menu *pipe_parent = ctx->item->parent;
	if (!pipe_parent) {
		wlr_log(WLR_ERROR, "[pipemenu %ld] invalid parent",
			(long)ctx->pid);
		return;
	}
	if (!pipe_parent->scene_tree->node.enabled) {
		wlr_log(WLR_ERROR, "[pipemenu %ld] parent menu already closed",
			(long)ctx->pid);
		return;
	}

	/*
	 * Pipemenus do not contain a toplevel <menu> element so we have to
	 * create that first `struct menu`.
	 */
	struct menu *pipe_menu = menu_create(ctx->server, ctx->item->id, /*label*/ NULL);
	pipe_menu->is_pipemenu = true;
	pipe_menu->triggered_by_view = pipe_parent->triggered_by_view;
	pipe_menu->parent = pipe_parent;

	menu_level++;
	current_menu = pipe_menu;
	if (!parse_buf(ctx->server, &ctx->buf)) {
		menu_free(pipe_menu);
		ctx->item->submenu = NULL;
		goto restore_menus;
	}
	ctx->item->submenu = pipe_menu;

	/*
	 * TODO: refactor validate() and post_processing() to only
	 * operate from current point onwards
	 */

	/* Set menu-widths before configuring */
	post_processing(ctx->server);

	/*
	 * TODO:
	 * (1) Combine this with the code in get_submenu_position()
	 *     and/or menu_configure()
	 * (2) Take into account menu_overlap_{x,y}
	 */
	enum menu_align align = ctx->item->parent->align;
	int x = pipe_parent->scene_tree->node.x;
	int y = pipe_parent->scene_tree->node.y + ctx->item->tree->node.y;
	if (align & LAB_MENU_OPEN_RIGHT) {
		x += pipe_parent->size.width;
	}
	menu_configure(pipe_menu, x, y, align);

	validate(ctx->server);

	/* Finally open the new submenu tree */
	wlr_scene_node_set_enabled(&pipe_menu->scene_tree->node, true);
	pipe_parent->selection.menu = pipe_menu;

restore_menus:
	current_menu = pipe_parent;
	menu_level--;
}

static void
pipemenu_ctx_destroy(struct pipe_context *ctx)
{
	wl_event_source_remove(ctx->event_read);
	wl_event_source_remove(ctx->event_timeout);
	spawn_piped_close(ctx->pid, ctx->pipe_fd);
	buf_reset(&ctx->buf);
	free(ctx);
	waiting_for_pipe_menu = false;
}

static int
handle_pipemenu_timeout(void *_ctx)
{
	struct pipe_context *ctx = _ctx;
	wlr_log(WLR_ERROR, "[pipemenu %ld] timeout reached, killing %s",
		(long)ctx->pid, ctx->item->execute);
	kill(ctx->pid, SIGTERM);
	pipemenu_ctx_destroy(ctx);
	return 0;
}

static bool
starts_with_less_than(const char *s)
{
	return (s + strspn(s, " \t\r\n"))[0] == '<';
}

static int
handle_pipemenu_readable(int fd, uint32_t mask, void *_ctx)
{
	struct pipe_context *ctx = _ctx;
	/* two 4k pages + 1 NULL byte */
	char data[8193];
	ssize_t size;

	do {
		/* leave space for terminating NULL byte */
		size = read(fd, data, sizeof(data) - 1);
	} while (size == -1 && errno == EINTR);

	if (size == -1) {
		wlr_log_errno(WLR_ERROR, "[pipemenu %ld] failed to read data (%s)",
			(long)ctx->pid, ctx->item->execute);
		goto clean_up;
	}

	/* Limit pipemenu buffer to 1 MiB for safety */
	if (ctx->buf.len + size > PIPEMENU_MAX_BUF_SIZE) {
		wlr_log(WLR_ERROR, "[pipemenu %ld] too big (> %d bytes); killing %s",
			(long)ctx->pid, PIPEMENU_MAX_BUF_SIZE, ctx->item->execute);
		kill(ctx->pid, SIGTERM);
		goto clean_up;
	}

	wlr_log(WLR_DEBUG, "[pipemenu %ld] read %ld bytes of data", (long)ctx->pid, size);
	if (size) {
		data[size] = '\0';
		buf_add(&ctx->buf, data);
		return 0;
	}

	/* Guard against badly formed data such as binary input */
	if (!starts_with_less_than(ctx->buf.data)) {
		wlr_log(WLR_ERROR, "expect xml data to start with '<'; abort pipemenu");
		goto clean_up;
	}

	create_pipe_menu(ctx);

clean_up:
	pipemenu_ctx_destroy(ctx);
	return 0;
}

static void
parse_pipemenu(struct menuitem *item)
{
	if (!is_unique_id(item->parent->server, item->id)) {
		wlr_log(WLR_ERROR, "duplicate id '%s'; abort pipemenu", item->id);
		return;
	}

	int pipe_fd = 0;
	pid_t pid = spawn_piped(item->execute, &pipe_fd);
	if (pid <= 0) {
		wlr_log(WLR_ERROR, "Failed to spawn pipe menu process %s", item->execute);
		return;
	}

	waiting_for_pipe_menu = true;
	struct pipe_context *ctx = znew(*ctx);
	ctx->server = item->parent->server;
	ctx->item = item;
	ctx->pid = pid;
	ctx->pipe_fd = pipe_fd;
	ctx->buf = BUF_INIT;

	ctx->event_read = wl_event_loop_add_fd(ctx->server->wl_event_loop,
		pipe_fd, WL_EVENT_READABLE, handle_pipemenu_readable, ctx);

	ctx->event_timeout = wl_event_loop_add_timer(ctx->server->wl_event_loop,
		handle_pipemenu_timeout, ctx);
	wl_event_source_timer_update(ctx->event_timeout, PIPEMENU_TIMEOUT_IN_MS);

	wlr_log(WLR_DEBUG, "[pipemenu %ld] executed: %s", (long)ctx->pid, ctx->item->execute);
}

static void
menu_process_item_selection(struct menuitem *item)
{
	assert(item);

	/* Do not keep selecting the same item */
	if (item == selected_item) {
		return;
	}

	if (waiting_for_pipe_menu) {
		return;
	}
	selected_item = item;

	if (!item->selectable) {
		return;
	}

	/* We are on an item that has new focus */
	menu_set_selection(item->parent, item);
	if (item->parent->selection.menu) {
		/* Close old submenu tree */
		menu_close(item->parent->selection.menu);
	}

	/* Pipemenu */
	if (item->execute && !item->submenu) {
		/* pipemenus are generated async */
		parse_pipemenu(item);
		return;
	}

	if (item->submenu) {
		/* Sync the triggering view */
		item->submenu->triggered_by_view = item->parent->triggered_by_view;
		/* Ensure the submenu has its parent set correctly */
		item->submenu->parent = item->parent;
		/* And open the new submenu tree */
		wlr_scene_node_set_enabled(
			&item->submenu->scene_tree->node, true);
	}

	item->parent->selection.menu = item->submenu;
}

/* Get the deepest submenu with active item selection or the root menu itself */
static struct menu *
get_selection_leaf(struct server *server)
{
	struct menu *menu = server->menu_current;
	if (!menu) {
		return NULL;
	}

	while (menu->selection.menu) {
		if (!menu->selection.menu->selection.item) {
			return menu;
		}
		menu = menu->selection.menu;
	}

	return menu;
}

/* Selects the next or previous sibling of the currently selected item */
static void
menu_item_select(struct server *server, bool forward)
{
	struct menu *menu = get_selection_leaf(server);
	if (!menu) {
		return;
	}

	struct menuitem *item = NULL;
	struct menuitem *selection = menu->selection.item;
	struct wl_list *start = selection ? &selection->link : &menu->menuitems;
	struct wl_list *current = start;
	while (!item || !item->selectable) {
		current = forward ? current->next : current->prev;
		if (current == start) {
			return;
		}
		if (current == &menu->menuitems) {
			/* Allow wrap around */
			item = NULL;
			continue;
		}
		item = wl_container_of(current, item, link);
	}

	menu_process_item_selection(item);
}

static bool
menu_execute_item(struct menuitem *item)
{
	assert(item);

	if (item->submenu || !item->selectable) {
		/* We received a click on a separator or item that just opens a submenu */
		return false;
	}

	/*
	 * We close the menu here to provide a faster feedback to the user.
	 * We do that without resetting the input state so src/cursor.c
	 * can do its own clean up on the following RELEASE event.
	 */
	struct server *server = item->parent->server;
	menu_close(server->menu_current);
	server->input_mode = LAB_INPUT_STATE_PASSTHROUGH;
	cursor_update_focus(server);

	/*
	 * We call the actions after closing the menu so that virtual keyboard
	 * input is sent to the focused_surface instead of being absorbed by the
	 * menu. Consider for example: `wlrctl keyboard type abc`
	 *
	 * We cannot call menu_close_root() directly here because it does both
	 * menu_close() and destroy_pipemenus() which we have to handle
	 * before/after action_run() respectively.
	 */
	actions_run(item->parent->triggered_by_view, server, &item->actions, 0);

	server->menu_current = NULL;
	destroy_pipemenus(server);
	return true;
}

/* Keyboard based selection */
void
menu_item_select_next(struct server *server)
{
	menu_item_select(server, /* forward */ true);
}

void
menu_item_select_previous(struct server *server)
{
	menu_item_select(server, /* forward */ false);
}

bool
menu_call_selected_actions(struct server *server)
{
	struct menu *menu = get_selection_leaf(server);
	if (!menu || !menu->selection.item) {
		return false;
	}

	return menu_execute_item(menu->selection.item);
}

/* Selects the first item on the submenu attached to the current selection */
void
menu_submenu_enter(struct server *server)
{
	struct menu *menu = get_selection_leaf(server);
	if (!menu || !menu->selection.menu) {
		return;
	}

	struct wl_list *start = &menu->selection.menu->menuitems;
	struct wl_list *current = start;
	struct menuitem *item = NULL;
	while (!item || !item->selectable) {
		current = current->next;
		if (current == start) {
			return;
		}
		item = wl_container_of(current, item, link);
	}

	menu_process_item_selection(item);
}

/* Re-selects the selected item on the parent menu of the current selection */
void
menu_submenu_leave(struct server *server)
{
	struct menu *menu = get_selection_leaf(server);
	if (!menu || !menu->parent || !menu->parent->selection.item) {
		return;
	}

	menu_process_item_selection(menu->parent->selection.item);
}

/* Mouse based selection */
void
menu_process_cursor_motion(struct wlr_scene_node *node)
{
	assert(node && node->data);
	struct menuitem *item = node_menuitem_from_node(node);
	menu_process_item_selection(item);
}

bool
menu_call_actions(struct wlr_scene_node *node)
{
	assert(node && node->data);
	struct menuitem *item = node_menuitem_from_node(node);

	return menu_execute_item(item);
}

void
menu_close_root(struct server *server)
{
	assert(server->input_mode == LAB_INPUT_STATE_MENU);
	if (server->menu_current) {
		menu_close(server->menu_current);
		server->menu_current = NULL;
		destroy_pipemenus(server);
	}
	server->input_mode = LAB_INPUT_STATE_PASSTHROUGH;
}

void
menu_reconfigure(struct server *server)
{
	menu_finish(server);
	server->menu_current = NULL;
	menu_init(server);
}
