/* ************************************************************************
*   File: class.c                                         EmpireMUD 2.0b3 *
*  Usage: code related to classes, including DB and OLC                   *
*                                                                         *
*  EmpireMUD code base by Paul Clarke, (C) 2000-2015                      *
*  All rights reserved.  See license.doc for complete information.        *
*                                                                         *
*  EmpireMUD based upon CircleMUD 3.0, bpl 17, by Jeremy Elson.           *
*  CircleMUD (C) 1993, 94 by the Trustees of the Johns Hopkins University *
*  CircleMUD is based on DikuMUD, Copyright (C) 1990, 1991.               *
************************************************************************ */

#include "conf.h"
#include "sysdep.h"

#include "structs.h"
#include "utils.h"
#include "comm.h"
#include "interpreter.h"
#include "handler.h"
#include "db.h"
#include "skills.h"
#include "olc.h"

/**
* Contents:
*   Helpers
*   Utilities
*   Database
*   Character Creation
*   OLC Handlers
*   Displays
*   Edit Modules
*   Commands
*/

// local data
const char *default_class_name = "Unnamed Class";
const char *default_class_abbrev = "???";

// local protos
void get_class_ability_display(struct class_ability *list, char *save_buffer, char_data *info_ch);
void get_class_skill_display(struct class_skill_req *list, char *save_buffer, bool one_line);

// external consts
extern const char *class_flags[];
extern const char *class_role[NUM_ROLES];
extern const char *class_role_color[NUM_ROLES];
extern const char *pool_types[];

// external funcs


 /////////////////////////////////////////////////////////////////////////////
//// HELPERS ////////////////////////////////////////////////////////////////

/**
* Checks that the player has the correct set of class abilities, and removes
* any they shouldn't have. This function ignores immortals.
*
* @param char_data *ch The player to check.
* @param class_data *cls Any player class, or NULL to detect fomr the player.
* @param int roole Any ROLE_X const, or NOTHING to detect from the player.
*/
void assign_class_abilities(char_data *ch, class_data *cls, int role) {
	void check_skill_sell(char_data *ch, any_vnum abil);
	
	ability_data *abil, *next_abil;
	struct class_ability *clab;
	bool has;

	// simple sanity
	if (IS_NPC(ch) || IS_IMMORTAL(ch)) {
		return;
	}
	
	// verify we have a class and role (defaults)
	if (!cls) {
		cls = GET_CLASS(ch);
	}
	if (role == NOTHING) {
		role = GET_CLASS_ROLE(ch);
	}
	
	// check all abilities
	HASH_ITER(hh, ability_table, abil, next_abil) {
		if (ABIL_ASSIGNED_SKILL(abil)) {
			continue;	// class abils only
		}
		
		// determine if the player's class/role has this abil
		has = FALSE;
		if (cls) {
			LL_FOREACH(CLASS_ABILITIES(cls), clab) {
				if (clab->role != role) {
					continue;	// wrong role
				}
				if (clab->vnum != ABIL_VNUM(abil)) {
					continue;	// wrong ability
				}
				
				// found it!
				has = TRUE;
				break;
			}
		}
		
		// remove any they shouldn't have
		if (has_ability(ch, ABIL_VNUM(abil)) && !has) {
			remove_ability(ch, ABIL_VNUM(abil), FALSE);
			check_skill_sell(ch, ABIL_VNUM(abil));
		}
		// add if needed
		if (has) {
			add_ability(ch, abil, FALSE);
		}
	}
}


/**
* Audits classes on startup. Erroring classes are set IN-DEVELOPMENT.
*/
void check_classes(void) {
	struct class_skill_req *clsk, *next_clsk;
	struct class_ability *clab, *next_clab;
	class_data *cls, *next_cls;
	bool error;
	
	HASH_ITER(hh, class_table, cls, next_cls) {
		error = FALSE;
		
		LL_FOREACH_SAFE(CLASS_SKILL_REQUIREMENTS(cls), clsk, next_clsk) {
			if (!find_skill_by_vnum(clsk->vnum)) {
				log("- Class [%d] %s has invalid skill %d requirement", CLASS_VNUM(cls), CLASS_NAME(cls), clsk->vnum);
				error = TRUE;
				LL_DELETE(CLASS_SKILL_REQUIREMENTS(cls), clsk);
				free(clsk);
			}
		}
		LL_FOREACH_SAFE(CLASS_ABILITIES(cls), clab, next_clab) {
			if (!find_ability_by_vnum(clab->vnum)) {
				log("- Class [%d] %s has invalid ability %d", CLASS_VNUM(cls), CLASS_NAME(cls), clab->vnum);
				error = TRUE;
				LL_DELETE(CLASS_ABILITIES(cls), clab);
				free(clab);
			}
		}
		
		if (error) {
			SET_BIT(CLASS_FLAGS(cls), CLASSF_IN_DEVELOPMENT);
		}
	}
}


/**
* Finds a class by ambiguous argument, which may be a vnum or a name.
* Names are matched by exact match first, or by multi-abbrev.
*
* @param char *argument The user input.
* @return class_data* The class, or NULL if it doesn't exist.
*/
class_data *find_class(char *argument) {
	class_data *cls;
	any_vnum vnum;
	
	if (isdigit(*argument) && (vnum = atoi(argument)) && (cls = find_class_by_vnum(vnum))) {
		return cls;
	}
	else {
		return find_class_by_name(argument);
	}
}


/**
* Look up a class by multi-abbrev, preferring exact matches.
*
* @param char *name The class name to look up.
* @return class_data* The class, or NULL if it doesn't exist.
*/
class_data *find_class_by_name(char *name) {
	class_data *cls, *next_cls, *partial = NULL;
	
	HASH_ITER(sorted_hh, sorted_classes, cls, next_cls) {
		// matches:
		if (!str_cmp(name, CLASS_NAME(cls))) {
			// perfect match
			return cls;
		}
		if (is_multiword_abbrev(name, CLASS_NAME(cls))) {
			// probable match
			partial = cls;
		}
	}
	
	// no exact match...
	return partial;
}


/**
* @param any_vnum vnum Any class vnum
* @return class_data* The class, or NULL if it doesn't exist
*/
class_data *find_class_by_vnum(any_vnum vnum) {
	class_data *cls;
	
	if (vnum < 0 || vnum == NOTHING) {
		return NULL;
	}
	
	HASH_FIND_INT(class_table, &vnum, cls);
	return cls;
}


/**
* This function updates a player's class and skill levelability data based
* on current skill levels.
*
* @param char_data *ch the player
*/
void update_class(char_data *ch) {
	#define NUM_BEST  3
	#define IGNORE_BOTTOM_SKILL_POINTS  30	// amount newbies should start with
	#define BEST_SUM_REQUIRED_FOR_100  (2 * CLASS_SKILL_CAP + SPECIALTY_SKILL_CAP)
	#define CLASS_LEVEL_BUFFER  25	// allows the class when still this much under the final level requirement
	
	int at_zero, over_basic, over_specialty, old_level, best_class_count, class_count;
	int best[NUM_BEST], best_level[NUM_BEST], best_iter, best_sub, best_total;
	class_data *class, *next_class, *old_class, *best_class;
	struct player_skill_data *skdata, *next_skdata;
	skill_data *skill, *next_skill;
	struct class_skill_req *csr;
	bool ok;
	
	if (!ch || IS_NPC(ch)) {
		return;
	}
	
	// init
	for (best_iter = 0; best_iter < NUM_BEST; ++best_iter) {
		best[best_iter] = NO_SKILL;
		best_level[best_iter] = 0;
	}
	over_basic = 0, over_specialty = 0;
	at_zero = 0;
	HASH_ITER(hh, skill_table, skill, next_skill) {
		if (!SKILL_FLAGGED(skill, SKILLF_IN_DEVELOPMENT)) {
			++at_zero;	// count total live skills
		}
	}
	
	// find skill counts
	HASH_ITER(hh, GET_SKILL_HASH(ch), skdata, next_skdata) {
		if (skdata->level > 0) {
			--at_zero;
		}
		if (skdata->level > BASIC_SKILL_CAP) {
			++over_basic;
		}
		if (skdata->level > SPECIALTY_SKILL_CAP) {
			++over_specialty;
		}
		
		// update best
		for (best_iter = 0; best_iter < NUM_BEST; ++best_iter) {
			// new best
			if (skdata->level > best_level[best_iter]) {
				// move down the other best first
				for (best_sub = NUM_BEST - 1; best_sub > best_iter; --best_sub) {
					best[best_sub] = best[best_sub-1];
					best_level[best_sub] = best_level[best_sub-1];
				}
				
				// store this one
				best[best_iter] = skdata->vnum;
				best_level[best_iter] = skdata->level;
				
				// ONLY update the first matching best
				break;
			}
		}
	}
	
	// set up skill limits:
	
	// can still gain new skills (gain from 0) if either you have more zeroes than required for bonus skills, or you're not using bonus skills
	CAN_GAIN_NEW_SKILLS(ch) = (at_zero > ZEROES_REQUIRED_FOR_BONUS_SKILLS) || (over_basic <= NUM_SPECIALTY_SKILLS_ALLOWED);
	
	// you qualify for bonus skills so long as you have enough skills at zero
	CAN_GET_BONUS_SKILLS(ch) = (at_zero >= ZEROES_REQUIRED_FOR_BONUS_SKILLS);
	
	old_class = GET_CLASS(ch);
	old_level = GET_SKILL_LEVEL(ch);

	// determine class
	best_class = NULL;
	best_class_count = 0;
	HASH_ITER(hh, class_table, class, next_class) {
		if (CLASS_FLAGGED(class, CLASSF_IN_DEVELOPMENT)) {
			continue;
		}
		
		// check skill requirements
		ok = TRUE;
		class_count = 0;
		LL_FOREACH(CLASS_SKILL_REQUIREMENTS(class), csr) {
			if (get_skill_level(ch, csr->vnum) < csr->level - CLASS_LEVEL_BUFFER || get_skill_level(ch, csr->vnum) > csr->level) {
				ok = FALSE;	// skill does not match
				break;
			}
			
			// found a matching skill
			++class_count;
		}
		
		// mark as match?
		if (ok && (!best_class || class_count > best_class_count)) {
			best_class = class;
			best_class_count = class_count;
		}
	}
	
	// clear role?
	if (best_class != old_class) {
		GET_CLASS_ROLE(ch) = ROLE_NONE;
	}
	
	GET_CLASS(ch) = best_class;
	assign_class_abilities(ch, NULL, NOTHING);
			
	// total up best X skills
	best_total = 0;
	for (best_iter = 0; best_iter < NUM_BEST; ++best_iter) {
		best_total += best_level[best_iter];
	}
	
	// set level
	GET_SKILL_LEVEL(ch) = (best_total - IGNORE_BOTTOM_SKILL_POINTS) * 100 / MAX(1, BEST_SUM_REQUIRED_FOR_100 - IGNORE_BOTTOM_SKILL_POINTS);
	GET_SKILL_LEVEL(ch) = MIN(CLASS_SKILL_CAP, MAX(1, GET_SKILL_LEVEL(ch)));
	
	// set progression (% of the way from 75 to 100)
	if (best_class && GET_SKILL_LEVEL(ch) >= SPECIALTY_SKILL_CAP) {
		// class progression level based on % of the way
		GET_CLASS_PROGRESSION(ch) = (GET_SKILL_LEVEL(ch) - SPECIALTY_SKILL_CAP) * 100 / (CLASS_SKILL_CAP - SPECIALTY_SKILL_CAP);
	}
	else {
		GET_CLASS_PROGRESSION(ch) = 0;
	}
	
	if (GET_CLASS(ch) != old_class || GET_SKILL_LEVEL(ch) != old_level) {
		affect_total(ch);
	}
}


 //////////////////////////////////////////////////////////////////////////////
//// UTILITIES ///////////////////////////////////////////////////////////////

/**
* Checks for common class problems and reports them to ch.
*
* @param class_data *cls The item to audit.
* @param char_data *ch The person to report to.
* @return bool TRUE if any problems were reported; FALSE if all good.
*/
bool audit_class(class_data *cls, char_data *ch) {
	bool problem = FALSE;
	
	if (CLASS_FLAGGED(cls, CLASSF_IN_DEVELOPMENT)) {
		olc_audit_msg(ch, CLASS_VNUM(cls), "IN-DEVELOPMENT");
		problem = TRUE;
	}
	
	return problem;
}


/**
* For the .list command.
*
* @param class_data *cls The thing to list.
* @param bool detail If TRUE, provide additional details
* @return char* The line to show (without a CRLF).
*/
char *list_one_class(class_data *cls, bool detail) {
	static char output[MAX_STRING_LENGTH];
	char lbuf[MAX_STRING_LENGTH];
	
	if (detail) {
		get_class_skill_display(CLASS_SKILL_REQUIREMENTS(cls), lbuf, TRUE);
		snprintf(output, sizeof(output), "[%5d] %s - %s", CLASS_VNUM(cls), CLASS_NAME(cls), lbuf);
	}
	else {
		snprintf(output, sizeof(output), "[%5d] %s", CLASS_VNUM(cls), CLASS_NAME(cls));
	}
		
	return output;
}


/**
* Searches for all uses of an class and displays them.
*
* @param char_data *ch The player.
* @param any_vnum vnum The class vnum.
*/
void olc_search_class(char_data *ch, any_vnum vnum) {
	char buf[MAX_STRING_LENGTH];
	class_data *cls = find_class_by_vnum(vnum);
	int size, found;
	
	if (!cls) {
		msg_to_char(ch, "There is no class %d.\r\n", vnum);
		return;
	}
	
	found = 0;
	size = snprintf(buf, sizeof(buf), "Occurrences of class %d (%s):\r\n", vnum, CLASS_NAME(cls));
	
	// classes are not actually used anywhere else
	
	if (found > 0) {
		size += snprintf(buf + size, sizeof(buf) - size, "%d location%s shown\r\n", found, PLURAL(found));
	}
	else {
		size += snprintf(buf + size, sizeof(buf) - size, " none\r\n");
	}
	
	page_string(ch->desc, buf, TRUE);
}


/**
* Delete by vnum: class abilities
*
* @param struct class_ability **list Pointer to a linked list of class abilities.
* @param any_vnum vnum The vnum to delete from that list.
* @return bool TRUE if it deleted at least one, FALSE if it wasn't in the list.
*/
bool remove_vnum_from_class_abilities(struct class_ability **list, any_vnum vnum) {
	struct class_ability *iter, *next_iter;
	bool found = FALSE;
	
	LL_FOREACH_SAFE(*list, iter, next_iter) {
		if (iter->vnum == vnum) {
			LL_DELETE(*list, iter);
			free(iter);
			found = TRUE;
		}
	}
	
	return found;
}


/**
* Delete by vnum: class skill requirements
*
* @param struct class_skill_req **list Pointer to a linked list of class skills.
* @param any_vnum vnum The vnum to delete from that list.
* @return bool TRUE if it deleted at least one, FALSE if it wasn't in the list.
*/
bool remove_vnum_from_class_skill_reqs(struct class_skill_req **list, any_vnum vnum) {
	struct class_skill_req *iter, *next_iter;
	bool found = FALSE;
	
	LL_FOREACH_SAFE(*list, iter, next_iter) {
		if (iter->vnum == vnum) {
			LL_DELETE(*list, iter);
			free(iter);
			found = TRUE;
		}
	}
	
	return found;
}


/**
* Sorts class abilities by role then name.
*
* @param struct class_ability *a First element.
* @param struct class_ability *b Second element.
* @return int sort code
*/
int sort_class_abilities(struct class_ability *a, struct class_ability *b) {
	ability_data *a_abil, *b_abil;
	
	if (a->role != b->role) {
		return a->role - b->role;
	}
	else {
		a_abil = find_ability_by_vnum(a->vnum);
		b_abil = find_ability_by_vnum(b->vnum);
		
		if (a_abil && b_abil) {
			return strcmp(ABIL_NAME(a_abil), ABIL_NAME(b_abil));
		}
		else if (a_abil) {
			return -1;
		}
		else {
			return 1;
		}
	}
}


// Simple vnum sorter for the class hash
int sort_classes(class_data *a, class_data *b) {
	return CLASS_VNUM(a) - CLASS_VNUM(b);
}


// alphabetic sorter for sorted_classes
int sort_classes_by_data(class_data *a, class_data *b) {
	return strcmp(NULLSAFE(CLASS_NAME(a)), NULLSAFE(CLASS_NAME(b)));
}


 //////////////////////////////////////////////////////////////////////////////
//// DATABASE ////////////////////////////////////////////////////////////////

/**
* Puts a class into the hash table.
*
* @param class_data *cls The class data to add to the table.
*/
void add_class_to_table(class_data *cls) {
	class_data *find;
	any_vnum vnum;
	
	if (cls) {
		vnum = CLASS_VNUM(cls);
		HASH_FIND_INT(class_table, &vnum, find);
		if (!find) {
			HASH_ADD_INT(class_table, vnum, cls);
			HASH_SORT(class_table, sort_classes);
		}
		
		// sorted table
		HASH_FIND(sorted_hh, sorted_classes, &vnum, sizeof(int), find);
		if (!find) {
			HASH_ADD(sorted_hh, sorted_classes, vnum, sizeof(int), cls);
			HASH_SRT(sorted_hh, sorted_classes, sort_classes_by_data);
		}
	}
}


/**
* Removes a class from the hash table.
*
* @param class_data *cls The class data to remove from the table.
*/
void remove_class_from_table(class_data *cls) {
	HASH_DEL(class_table, cls);
	HASH_DELETE(sorted_hh, sorted_classes, cls);
}


/**
* Initializes a new class. This clears all memory for it, so set the vnum
* AFTER.
*
* @param class_data *cls The class to initialize.
*/
void clear_class(class_data *cls) {
	extern const int base_player_pools[NUM_POOLS];
	
	memset((char *) cls, 0, sizeof(class_data));
	int iter;
	
	CLASS_VNUM(cls) = NOTHING;
	
	for (iter = 0; iter < NUM_POOLS; ++iter) {
		CLASS_POOL(cls, iter) = base_player_pools[iter];
	}
}


/**
* Duplicates a list of class abilities, for editing.
*
* @param struct class_ability *input The head of the list to copy.
* @return struct class_ability* The copied list.
*/
struct class_ability *copy_class_abilities(struct class_ability *input) {
	struct class_ability *el, *iter, *list = NULL;
	
	LL_FOREACH(input, iter) {
		CREATE(el, struct class_ability, 1);
		*el = *iter;
		el->next = NULL;
		LL_APPEND(list, el);
	}
	
	return list;
}


/**
* Duplicates a list of class skill requirements, for editing.
*
* @param struct class_skill_req *input The head of the list to copy.
* @return struct class_skill_req* The copied list.
*/
struct class_skill_req *copy_class_skill_reqs(struct class_skill_req *input) {
	struct class_skill_req *el, *iter, *list = NULL;
	
	LL_FOREACH(input, iter) {
		CREATE(el, struct class_skill_req, 1);
		*el = *iter;
		el->next = NULL;
		LL_APPEND(list, el);
	}
	
	return list;
}


/**
* @param struct class_ability *list Frees the memory for this list.
*/
void free_class_abilities(struct class_ability *list) {
	struct class_ability *tmp, *next;
	
	LL_FOREACH_SAFE(list, tmp, next) {
		free(tmp);
	}
}


/**
* @param struct class_skill_req *list Frees the memory for this list.
*/
void free_class_skill_reqs(struct class_skill_req *list) {
	struct class_skill_req *tmp, *next;
	
	LL_FOREACH_SAFE(list, tmp, next) {
		free(tmp);
	}
}


/**
* frees up memory for a class data item.
*
* See also: olc_delete_class
*
* @param class_data *cls The class data to free.
*/
void free_class(class_data *cls) {
	class_data *proto = find_class_by_vnum(CLASS_VNUM(cls));
	
	if (CLASS_NAME(cls) && (!proto || CLASS_NAME(cls) != CLASS_NAME(proto))) {
		free(CLASS_NAME(cls));
	}
	if (CLASS_ABBREV(cls) && (!proto || CLASS_ABBREV(cls) != CLASS_ABBREV(proto))) {
		free(CLASS_ABBREV(cls));
	}
	
	if (CLASS_ABILITIES(cls) && (!proto || CLASS_ABILITIES(cls) != CLASS_ABILITIES(proto))) {
		free_class_abilities(CLASS_ABILITIES(cls));
	}
	if (CLASS_SKILL_REQUIREMENTS(cls) && (!proto || CLASS_SKILL_REQUIREMENTS(cls) != CLASS_SKILL_REQUIREMENTS(proto))) {
		free_class_skill_reqs(CLASS_SKILL_REQUIREMENTS(cls));
	}
	
	free(cls);
}


/**
* Read one class from file.
*
* @param FILE *fl The open .class file
* @param any_vnum vnum The class vnum
*/
void parse_class(FILE *fl, any_vnum vnum) {
	char line[256], error[256], str_in[256];
	struct class_skill_req *skill;
	struct class_ability *abil;
	class_data *cls, *find;
	int int_in[4], iter;
	
	CREATE(cls, class_data, 1);
	clear_class(cls);
	CLASS_VNUM(cls) = vnum;
	
	HASH_FIND_INT(class_table, &vnum, find);
	if (find) {
		log("WARNING: Duplicate class vnum #%d", vnum);
		// but have to load it anyway to advance the file
	}
	add_class_to_table(cls);
		
	// for error messages
	sprintf(error, "class vnum %d", vnum);
	
	// lines 1-2: name, abbrev
	CLASS_NAME(cls) = fread_string(fl, error);
	CLASS_ABBREV(cls) = fread_string(fl, error);
	
	// line 3: flags
	if (!get_line(fl, line) || sscanf(line, "%s", str_in) != 1) {
		log("SYSERR: Format error in line 3 of %s", error);
		exit(1);
	}
	
	CLASS_FLAGS(cls) = asciiflag_conv(str_in);
	
	// line 4: pools
	if (!get_line(fl, line) || sscanf(line, "%d %d %d %d", &int_in[0], &int_in[1], &int_in[2], &int_in[3]) != NUM_POOLS) {
		log("SYSERR: Format error in line 4 of %s", error);
		exit(1);
	}
	for (iter = 0; iter < NUM_POOLS; ++iter) {
		CLASS_POOL(cls, iter) = int_in[iter];
	}
		
	// optionals
	for (;;) {
		if (!get_line(fl, line)) {
			log("SYSERR: Format error in %s, expecting alphabetic flags", error);
			exit(1);
		}
		switch (*line) {
			case 'A': {	// abilities
				if (sscanf(line, "A %d %d", &int_in[0], &int_in[1]) != 2) {
					log("SYSERR: Format error in line A of %s", error);
					exit(1);
				}
				
				CREATE(abil, struct class_ability, 1);
				abil->role = int_in[0];
				abil->vnum = int_in[1];
				LL_APPEND(CLASS_ABILITIES(cls), abil);
				break;
			}
			case 'R': {	// skill requirements
				if (sscanf(line, "R %d %d", &int_in[0], &int_in[1]) != 2) {
					log("SYSERR: Format error in line R of %s", error);
					exit(1);
				}
				
				CREATE(skill, struct class_skill_req, 1);
				skill->vnum = int_in[0];
				skill->level = int_in[1];
				LL_APPEND(CLASS_SKILL_REQUIREMENTS(cls), skill);
				break;
			}
			
			// end
			case 'S': {
				return;
			}
			
			default: {
				log("SYSERR: Format error in %s, expecting alphabetic flags", error);
				exit(1);
			}
		}
	}
}


// writes entries in the class index
void write_class_index(FILE *fl) {
	class_data *cls, *next_cls;
	int this, last;
	
	last = NO_WEAR;
	HASH_ITER(hh, class_table, cls, next_cls) {
		// determine "zone number" by vnum
		this = (int)(CLASS_VNUM(cls) / 100);
	
		if (this != last) {
			fprintf(fl, "%d%s\n", this, CLASS_SUFFIX);
			last = this;
		}
	}
}


/**
* Outputs one class in the db file format, starting with a #VNUM and
* ending with an S.
*
* @param FILE *fl The file to write it to.
* @param class_data *cls The thing to save.
*/
void write_class_to_file(FILE *fl, class_data *cls) {
	struct class_skill_req *skill;
	struct class_ability *abil;
	int iter;
	
	if (!fl || !cls) {
		syslog(SYS_ERROR, LVL_START_IMM, TRUE, "SYSERR: write_class_to_file called without %s", !fl ? "file" : "class");
		return;
	}
	
	fprintf(fl, "#%d\n", CLASS_VNUM(cls));
	
	// 1-2. strings
	fprintf(fl, "%s~\n", NULLSAFE(CLASS_NAME(cls)));
	fprintf(fl, "%s~\n", NULLSAFE(CLASS_ABBREV(cls)));
	
	// 3. flags
	fprintf(fl, "%s\n", bitv_to_alpha(CLASS_FLAGS(cls)));
	
	// 4. pools
	for (iter = 0; iter < NUM_POOLS; ++iter) {
		fprintf(fl, "%d ", CLASS_POOL(cls, iter));
	}
	fprintf(fl, "\n");
	
	// 'A': abilities
	LL_FOREACH(CLASS_ABILITIES(cls), abil) {
		fprintf(fl, "A %d %d\n", abil->role, abil->vnum);
	}
	
	// 'R': skill requirements
	LL_FOREACH(CLASS_SKILL_REQUIREMENTS(cls), skill) {
		fprintf(fl, "R %d %d\n", skill->vnum, skill->level);
	}
	
	// end
	fprintf(fl, "S\n");
}


 //////////////////////////////////////////////////////////////////////////////
//// OLC HANDLERS ////////////////////////////////////////////////////////////

/**
* Creates a new class entry.
* 
* @param any_vnum vnum The number to create.
* @return class_data* The new class's prototype.
*/
class_data *create_class_table_entry(any_vnum vnum) {
	class_data *cls;
	
	// sanity
	if (find_class_by_vnum(vnum)) {
		log("SYSERR: Attempting to insert class at existing vnum %d", vnum);
		return find_class_by_vnum(vnum);
	}
	
	CREATE(cls, class_data, 1);
	clear_class(cls);
	CLASS_VNUM(cls) = vnum;
	CLASS_NAME(cls) = str_dup(default_class_name);
	CLASS_ABBREV(cls) = str_dup(default_class_abbrev);
	add_class_to_table(cls);

	// save index and class file now
	save_index(DB_BOOT_CLASS);
	save_library_file_for_vnum(DB_BOOT_CLASS, vnum);

	return cls;
}


/**
* WARNING: This function actually deletes a class.
*
* @param char_data *ch The person doing the deleting.
* @param any_vnum vnum The vnum to delete.
*/
void olc_delete_class(char_data *ch, any_vnum vnum) {
	void update_class(char_data *ch);

	char_data *chiter;
	class_data *cls;
	
	if (!(cls = find_class_by_vnum(vnum))) {
		msg_to_char(ch, "There is no such class %d.\r\n", vnum);
		return;
	}
	
	// remove it from the hash table first
	remove_class_from_table(cls);
	
	// remove from live players
	LL_FOREACH(character_list, chiter) {
		if (IS_NPC(chiter)) {
			continue;
		}
		if (GET_CLASS(chiter) != cls) {
			continue;
		}
		update_class(chiter);
	}

	// save index and class file now
	save_index(DB_BOOT_CLASS);
	save_library_file_for_vnum(DB_BOOT_CLASS, vnum);
	
	syslog(SYS_OLC, GET_INVIS_LEV(ch), TRUE, "OLC: %s has deleted class %d", GET_NAME(ch), vnum);
	msg_to_char(ch, "Class %d deleted.\r\n", vnum);
	
	free_class(cls);
}


/**
* Function to save a player's changes to a class (or a new one).
*
* @param descriptor_data *desc The descriptor who is saving.
*/
void save_olc_class(descriptor_data *desc) {	
	class_data *proto, *cls = GET_OLC_CLASS(desc);
	any_vnum vnum = GET_OLC_VNUM(desc);
	UT_hash_handle hh, sorted;

	// have a place to save it?
	if (!(proto = find_class_by_vnum(vnum))) {
		proto = create_class_table_entry(vnum);
	}
	
	// free prototype strings and pointers
	if (CLASS_NAME(proto)) {
		free(CLASS_NAME(proto));
	}
	if (CLASS_ABBREV(proto)) {
		free(CLASS_ABBREV(proto));
	}
	free_class_abilities(CLASS_ABILITIES(proto));
	free_class_skill_reqs(CLASS_SKILL_REQUIREMENTS(proto));
	
	// sanity
	if (!CLASS_NAME(cls) || !*CLASS_NAME(cls)) {
		if (CLASS_NAME(cls)) {
			free(CLASS_NAME(cls));
		}
		CLASS_NAME(cls) = str_dup(default_class_name);
	}
	if (!CLASS_ABBREV(cls) || !*CLASS_ABBREV(cls)) {
		if (CLASS_ABBREV(cls)) {
			free(CLASS_ABBREV(cls));
		}
		CLASS_ABBREV(cls) = str_dup(default_class_abbrev);
	}

	// save data back over the proto-type
	hh = proto->hh;	// save old hash handle
	sorted = proto->sorted_hh;
	*proto = *cls;	// copy over all data
	proto->vnum = vnum;	// ensure correct vnum
	proto->hh = hh;	// restore old hash handle
	proto->sorted_hh = sorted;
		
	// and save to file
	save_library_file_for_vnum(DB_BOOT_CLASS, vnum);

	// ... and re-sort
	HASH_SRT(sorted_hh, sorted_classes, sort_classes_by_data);
}


/**
* Creates a copy of a class, or clears a new one, for editing.
* 
* @param class_data *input The class to copy, or NULL to make a new one.
* @return class_data* The copied class.
*/
class_data *setup_olc_class(class_data *input) {
	class_data *new;
	
	CREATE(new, class_data, 1);
	clear_class(new);
	
	if (input) {
		// copy normal data
		*new = *input;

		// copy things that are pointers
		CLASS_NAME(new) = CLASS_NAME(input) ? str_dup(CLASS_NAME(input)) : NULL;
		CLASS_ABBREV(new) = CLASS_ABBREV(input) ? str_dup(CLASS_ABBREV(input)) : NULL;
		
		// copy lists
		CLASS_ABILITIES(new) = copy_class_abilities(CLASS_ABILITIES(input));
		CLASS_SKILL_REQUIREMENTS(new) = copy_class_skill_reqs(CLASS_SKILL_REQUIREMENTS(input));
	}
	else {
		// brand new: some defaults
		CLASS_NAME(new) = str_dup(default_class_name);
		CLASS_ABBREV(new) = str_dup(default_class_abbrev);
		CLASS_FLAGS(new) = CLASSF_IN_DEVELOPMENT;
	}
	
	// done
	return new;	
}


 //////////////////////////////////////////////////////////////////////////////
//// DISPLAYS ////////////////////////////////////////////////////////////////

/**
* For vstat.
*
* @param char_data *ch The player requesting stats.
* @param class_data *cls The class to display.
*/
void do_stat_class(char_data *ch, class_data *cls) {
	char buf[MAX_STRING_LENGTH], part[MAX_STRING_LENGTH];
	size_t size;
	
	if (!cls) {
		return;
	}
	
	// first line
	size = snprintf(buf, sizeof(buf), "VNum: [\tc%d\t0], Name: \tc%s\t0, Abbrev: [\tc%s\t0]\r\n", CLASS_VNUM(cls), CLASS_NAME(cls), CLASS_ABBREV(cls));
	
	sprintbit(CLASS_FLAGS(cls), class_flags, part, TRUE);
	size += snprintf(buf + size, sizeof(buf) - size, "Flags: \tg%s\t0\r\n", part);
	
	get_class_skill_display(CLASS_SKILL_REQUIREMENTS(cls), part, FALSE);
	size += snprintf(buf + size, sizeof(buf) - size, "Skills required:\r\n%s", part);
	
	get_class_ability_display(CLASS_ABILITIES(cls), part, NULL);
	size += snprintf(buf + size, sizeof(buf) - size, "Roles and abilities:\r\n%s", part);

	page_string(ch->desc, buf, TRUE);
}


/**
* Gets the class role display for olc, stat, or other uses.
*
* @param struct class_ability *list The list of abilities to display.
* @param char *save_buffer A buffer to store the display to.
* @param char_data *info_ch Optional: highlights abilities this player has (or NULL).
*/
void get_class_ability_display(struct class_ability *list, char *save_buffer, char_data *info_ch) {
	int count = 0, last_role = -1;
	struct class_ability *iter;
	ability_data *abil;
	
	*save_buffer = '\0';
	
	// ensure sorting first, as this arranges the roles
	LL_SORT(list, sort_class_abilities);

	LL_FOREACH(list, iter) {
		if (iter->role != last_role) {
			sprintf(save_buffer + strlen(save_buffer), "%s %s%s\t0: ", last_role != -1 ? "\r\n" : "", class_role_color[iter->role], class_role[iter->role]);
			last_role = iter->role;
			count = 0;
		}
		
		if ((abil = find_ability_by_vnum(iter->vnum))) {
			sprintf(save_buffer + strlen(save_buffer), "%s%s%s\t0", (count++ > 0) ? ", " : "", (info_ch && has_ability(info_ch, iter->vnum)) ? "\tg" : "", ABIL_NAME(abil));
		}
		else {
			sprintf(save_buffer + strlen(save_buffer), "%s%d Unknown\t0", (count++ > 0) ? ", " : "", iter->vnum);
		}
	}
}


/**
* Gets the display for a list of skill requirements for a class.
*
* @param struct class_skill_req *list The list to display.
* @param char *save_buffer A string to save it in.
* @param bool one_line If TRUE, is a comma-separated line. Otherwise, a numbered list.
*/
void get_class_skill_display(struct class_skill_req *list, char *save_buffer, bool one_line) {
	char lbuf[MAX_STRING_LENGTH];
	struct class_skill_req *iter;
	int count = 0;
	
	*save_buffer = '\0';
	
	LL_FOREACH(list, iter) {
		snprintf(lbuf, sizeof(lbuf), "%s %d", get_skill_name_by_vnum(iter->vnum), iter->level);
		
		if (one_line) {
			sprintf(save_buffer + strlen(save_buffer), "%s%s", (*save_buffer ? ", " : ""), lbuf);
		}
		else {
			sprintf(save_buffer + strlen(save_buffer), "%2d. %s\r\n", ++count, lbuf);
		}
	}
	if (!list) {
		sprintf(save_buffer + strlen(save_buffer), "%snone%s", one_line ? "" : "  ", one_line ? "" : "\r\n");
	}
}


/**
* This is the main recipe display for class OLC. It displays the user's
* currently-edited class.
*
* @param char_data *ch The person who is editing a class and will see its display.
*/
void olc_show_class(char_data *ch) {
	class_data *cls = GET_OLC_CLASS(ch->desc);
	char buf[MAX_STRING_LENGTH], lbuf[MAX_STRING_LENGTH];
	
	if (!cls) {
		return;
	}
	
	*buf = '\0';
	
	sprintf(buf + strlen(buf), "[\tc%d\t0] \tc%s\t0\r\n", GET_OLC_VNUM(ch->desc), !find_class_by_vnum(CLASS_VNUM(cls)) ? "new class" : CLASS_NAME(find_class_by_vnum(CLASS_VNUM(cls))));
	sprintf(buf + strlen(buf), "<\tyname\t0> %s\r\n", NULLSAFE(CLASS_NAME(cls)));
	sprintf(buf + strlen(buf), "<\tyabbrev\t0> %s\r\n", NULLSAFE(CLASS_ABBREV(cls)));
	
	sprintbit(CLASS_FLAGS(cls), class_flags, lbuf, TRUE);
	sprintf(buf + strlen(buf), "<\tyflags\t0> %s\r\n", lbuf);
	
	get_class_skill_display(CLASS_SKILL_REQUIREMENTS(cls), lbuf, FALSE);
	sprintf(buf + strlen(buf), "Skills required: <\tyrequires\t0>\r\n%s", lbuf);
	
	get_class_ability_display(CLASS_ABILITIES(cls), lbuf, NULL);
	sprintf(buf + strlen(buf), "Class roles and abilities: <\tyrole\t0>\r\n%s", lbuf);
	
	page_string(ch->desc, buf, TRUE);
}


/**
* Searches the class db for a match, and prints it to the character.
*
* @param char *searchname The search string.
* @param char_data *ch The player who is searching.
* @return int The number of matches shown.
*/
int vnum_class(char *searchname, char_data *ch) {
	class_data *iter, *next_iter;
	int found = 0;
	
	HASH_ITER(hh, class_table, iter, next_iter) {
		if (multi_isname(searchname, CLASS_NAME(iter)) || multi_isname(searchname, CLASS_ABBREV(iter))) {
			msg_to_char(ch, "%3d. [%5d] %s\r\n", ++found, CLASS_VNUM(iter), CLASS_NAME(iter));
		}
	}
	
	return found;
}


 //////////////////////////////////////////////////////////////////////////////
//// OLC MODULES /////////////////////////////////////////////////////////////

OLC_MODULE(classedit_abbrev) {
	class_data *cls = GET_OLC_CLASS(ch->desc);
	
	if (color_strlen(argument) != 4) {
		msg_to_char(ch, "Class abbreviations must be 4 letters.\r\n");
	}
	else if (color_code_length(argument) > 0) {
		msg_to_char(ch, "Class abbreviations may not contain color codes.\r\n");
	}
	else {
		olc_process_string(ch, argument, "abbreviation", &CLASS_ABBREV(cls));
	}
}


OLC_MODULE(classedit_flags) {
	class_data *cls = GET_OLC_CLASS(ch->desc);
	bool had_indev = IS_SET(CLASS_FLAGS(cls), CLASSF_IN_DEVELOPMENT) ? TRUE : FALSE;
	
	CLASS_FLAGS(cls) = olc_process_flag(ch, argument, "class", "flags", class_flags, CLASS_FLAGS(cls));
	
	// validate removal of IN-DEVELOPMENT
	if (had_indev && !IS_SET(CLASS_FLAGS(cls), CLASSF_IN_DEVELOPMENT) && GET_ACCESS_LEVEL(ch) < LVL_UNRESTRICTED_BUILDER && !OLC_FLAGGED(ch, OLC_FLAG_CLEAR_IN_DEV)) {
		msg_to_char(ch, "You don't have permission to remove the IN-DEVELOPMENT flag.\r\n");
		SET_BIT(CLASS_FLAGS(cls), CLASSF_IN_DEVELOPMENT);
	}
}


OLC_MODULE(classedit_name) {
	class_data *cls = GET_OLC_CLASS(ch->desc);
	olc_process_string(ch, argument, "name", &CLASS_NAME(cls));
}


OLC_MODULE(classedit_requires) {
}


OLC_MODULE(classedit_role) {
}


 /////////////////////////////////////////////////////////////////////////////
//// COMMANDS ///////////////////////////////////////////////////////////////

ACMD(do_class) {
	void resort_empires();
	
	char arg2[MAX_INPUT_LENGTH], buf[MAX_STRING_LENGTH];
	empire_data *emp = GET_LOYALTY(ch);
	int iter, found;
	
	two_arguments(argument, arg, arg2);

	if (IS_NPC(ch)) {
		msg_to_char(ch, "You have no class!\r\n");
	}
	else if (*arg && !str_cmp(arg, "role")) {
		// Handle role selection or display
		
		if (GET_SKILL_LEVEL(ch) < CLASS_SKILL_CAP) {
			msg_to_char(ch, "You can't set a class role until you hit skill level %d.\r\n", CLASS_SKILL_CAP);
		}
		else if (!*arg2) {
			msg_to_char(ch, "Your class role is currently set to: %s.\r\n", class_role[(int) GET_CLASS_ROLE(ch)]);
		}
		else if (GET_POS(ch) < POS_STANDING) {
			msg_to_char(ch, "You can't change your class role right now!\r\n");
		}
		else {
			found = NOTHING;
			for (iter = 0; iter < NUM_ROLES && found == NOTHING; ++iter) {
				if (is_abbrev(arg2, class_role[iter])) {
					found = iter;
				}
			}
			
			if (found == NOTHING) {
				msg_to_char(ch, "Unknown role '%s'.\r\n", arg2);
			}
			else {
				// remove old abilities
				if (emp) {
					adjust_abilities_to_empire(ch, emp, FALSE);
				}
				
				// change role
				GET_CLASS_ROLE(ch) = found;
				
				// add new abilities
				assign_class_abilities(ch, NULL, NOTHING);
				if (emp) {
					adjust_abilities_to_empire(ch, emp, TRUE);
					resort_empires();
				}
				
				msg_to_char(ch, "Your class role is now: %s.\r\n", class_role[(int) GET_CLASS_ROLE(ch)]);
			}
		}
	}
	else if (*arg) {
		msg_to_char(ch, "Invalid class command.\r\n");
	}
	else {
		// Display class info
		
		if (!GET_CLASS(ch)) {
			msg_to_char(ch, "You don't have a class. You can earn your class by raising two skills to 76 or higher.\r\n");
		}
		else {
			msg_to_char(ch, "%s\r\nClass: %s%s (%s)\t0 %d/%d/%d\r\n", PERS(ch, ch, TRUE), class_role_color[GET_CLASS_ROLE(ch)], SHOW_CLASS_NAME(ch), class_role[(int) GET_CLASS_ROLE(ch)], GET_SKILL_LEVEL(ch), GET_GEAR_LEVEL(ch), GET_COMPUTED_LEVEL(ch));
			
			get_class_ability_display(CLASS_ABILITIES(GET_CLASS(ch)), buf, ch);
			msg_to_char(ch, " Available class roles:\r\n%s", *buf ? "" : "  none\r\n");
		}
	}
}
