/* Copyright (c) 1997-2001 Miller Puckette and others.
* For information on usage and redistribution, and for a DISCLAIMER OF ALL
* WARRANTIES, see the file, "LICENSE.txt," in this distribution.  */

/* this file defines the "glist" class, also known as "canvas" (the two used
to be different but are now unified except for some fossilized names.) */

#include <stdlib.h>
#include <stdio.h>
#include "m_pd.h"
#include "m_imp.h"
#include "s_stuff.h"
#include "g_magicglass.h"
#include "g_canvas.h"
#include "g_all_guis.h"
#include <string.h>

extern int do_not_redraw;
extern void canvas_drawconnection(t_canvas *x, int lx1, int ly1, int lx2, int ly2, t_int tag, int issignal);
extern void canvas_updateconnection(t_canvas *x, int lx1, int ly1, int lx2, int ly2, t_int tag);

    /* LATER consider adding font size to this struct (see glist_getfont()) */
struct _canvasenvironment
{
    t_symbol *ce_dir;      /* directory patch lives in */
    int ce_argc;           /* number of "$" arguments */
    t_atom *ce_argv;       /* array of "$" arguments */
    int ce_dollarzero;     /* value of "$0" */
    t_namelist *ce_path;   /* search path */
};

#define GLIST_DEFCANVASWIDTH 450
#define GLIST_DEFCANVASHEIGHT 300

#ifdef __APPLE__
#define GLIST_DEFCANVASYLOC 22
#else
#define GLIST_DEFCANVASYLOC 0
#endif

/* ---------------------- variables --------------------------- */

extern t_pd *newest;
t_class *canvas_class;
int canvas_dspstate;                /* whether DSP is on or off */  
t_canvas *canvas_editing;           /* last canvas to start text edting */ 
t_canvas *canvas_whichfind;         /* last canvas we did a find in */ 
t_canvas *canvas_list;              /* list of all root canvases */

/* ------------------ forward function declarations --------------- */
static void canvas_start_dsp(void);
static void canvas_stop_dsp(void);
static void canvas_drawlines(t_canvas *x);
void canvas_setbounds(t_canvas *x, int x1, int y1, int x2, int y2);
void canvas_reflecttitle(t_canvas *x);
static void canvas_addtolist(t_canvas *x);
static void canvas_takeofflist(t_canvas *x);
static void canvas_pop(t_canvas *x, t_floatarg fvis);
static int canvas_should_bind(t_canvas *x);
static void canvas_bind(t_canvas *x);
static void canvas_unbind(t_canvas *x);

/* --------- functions to handle the canvas environment ----------- */

static t_symbol *canvas_newfilename = &s_;
static t_symbol *canvas_newdirectory = &s_;
static int canvas_newargc;
static t_atom *canvas_newargv;

static void glist_doupdatewindowlist(t_glist *gl, char *sbuf)
{
    t_gobj *g;
    if (glist_amreloadingabstractions)  /* not if we're in a reload */
        return;
    if (!gl->gl_owner)
    {
        /* this is a canvas; if we have a window, put on "windows" list */
        t_canvas *canvas = (t_canvas *)gl;
        if (canvas->gl_havewindow)
        {
            if (strlen(sbuf) + strlen(gl->gl_name->s_name) + 100 <= 1024)
            {
                char tbuf[1024];
                sprintf(tbuf, "{{%s} .x%lx} ", gl->gl_name->s_name,
                    (t_int)canvas);
                strcat(sbuf, tbuf);
            }
        }
    }
    for (g = gl->gl_list; g; g = g->g_next)
    {
        if (pd_class(&g->g_pd) == canvas_class)
            glist_doupdatewindowlist((t_glist *)g, sbuf);
    }
    return;
}

    /* maintain the list of visible toplevels for the GUI's "windows" menu */
void canvas_updatewindowlist( void)
{
    t_canvas *x;
    char sbuf[1024];
    strcpy(sbuf, "set menu_windowlist {");
        /* find all root canvases */
    for (x = canvas_list; x; x = x->gl_next)
        glist_doupdatewindowlist(x, sbuf);
    /* next line updates the window menu state before -postcommand tries it */
    strcat(sbuf, "}\npdtk_fixwindowmenu\n");
    sys_gui(sbuf);
}

    /* add a glist the list of "root" canvases (toplevels without parents.) */
static void canvas_addtolist(t_canvas *x)
{
    x->gl_next = canvas_list;
    canvas_list = x;
}

static void canvas_takeofflist(t_canvas *x)
{
        /* take it off the window list */
    if (x == canvas_list) canvas_list = x->gl_next;
    else
    {
        t_canvas *z;
        for (z = canvas_list; z->gl_next != x; z = z->gl_next)
            ;
        z->gl_next = x->gl_next;
    }
}


void canvas_setargs(int argc, t_atom *argv)
{
        /* if there's an old one lying around free it here.  This
        happens if an abstraction is loaded but never gets as far
        as calling canvas_new(). */
    if (canvas_newargv)
        freebytes(canvas_newargv, canvas_newargc * sizeof(t_atom));
    canvas_newargc = argc;
    canvas_newargv = copybytes(argv, argc * sizeof(t_atom));
}

void glob_setfilename(void *dummy, t_symbol *filesym, t_symbol *dirsym)
{
    canvas_newfilename = filesym;
    canvas_newdirectory = dirsym;
}

t_canvas *canvas_getcurrent(void)
{
    return ((t_canvas *)pd_findbyclass(&s__X, canvas_class));
}

void canvas_setcurrent(t_canvas *x)
{
    pd_pushsym(&x->gl_pd);
}

void canvas_unsetcurrent(t_canvas *x)
{
    pd_popsym(&x->gl_pd);
}

t_canvasenvironment *canvas_getenv(t_canvas *x)
{
    if (!x) bug("canvas_getenv");
    while (!x->gl_env)
        if (!(x = x->gl_owner))
            bug("t_canvasenvironment");
    return (x->gl_env);
}

int canvas_getdollarzero( void)
{
    t_canvas *x = canvas_getcurrent();
    t_canvasenvironment *env = (x ? canvas_getenv(x) : 0);
    if (env)
        return (env->ce_dollarzero);
    else return (0);
}

void canvas_getargs(int *argcp, t_atom **argvp)
{
    t_canvasenvironment *e = canvas_getenv(canvas_getcurrent());
    *argcp = e->ce_argc;
    *argvp = e->ce_argv;
}

t_symbol *canvas_realizedollar(t_canvas *x, t_symbol *s)
{
    t_symbol *ret;
    char *name = s->s_name;
    if (strchr(name, '$'))
    {
        t_canvasenvironment *env = canvas_getenv(x);
        canvas_setcurrent(x);
        ret = binbuf_realizedollsym(s, env->ce_argc, env->ce_argv, 1);
        canvas_unsetcurrent(x);
    }
    else ret = s;
    return (ret);
}

t_symbol *canvas_getcurrentdir(void)
{
    t_canvasenvironment *e = canvas_getenv(canvas_getcurrent());
    return (e->ce_dir);
}

t_symbol *canvas_getdir(t_canvas *x)
{
    t_canvasenvironment *e = canvas_getenv(x);
    return (e->ce_dir);
}

void canvas_makefilename(t_canvas *x, char *file, char *result, int resultsize)
{
	char interim[FILENAME_MAX];
	sys_expandpathelems(file, interim);
	//fprintf(stderr,"interim = <%s>\n", interim);
    char *dir = canvas_getenv(x)->ce_dir->s_name;
    if (interim[0] == '/' || (interim[0] && interim[1] == ':') || !*dir)
    {
		//fprintf(stderr,"root file\n");
        strncpy(result, interim, resultsize);
        result[resultsize-1] = 0;
    }
    else
    {
		//fprintf(stderr,"relative file\n");
        int nleft;
        strncpy(result, dir, resultsize);
        result[resultsize-1] = 0;
        nleft = resultsize - strlen(result) - 1;
        if (nleft <= 0) return;
        strcat(result, "/");
        strncat(result, interim, nleft);
        result[resultsize-1] = 0;
    } 
	//fprintf(stderr,"resulting file = <%s>\n", result);          
}

void canvas_rename(t_canvas *x, t_symbol *s, t_symbol *dir)
{
    canvas_unbind(x);
    x->gl_name = s;
    canvas_bind(x);
    if (glist_isvisible(x))
    if (x->gl_havewindow) //was glist_isvisible(x)
        canvas_reflecttitle(x);
    if (dir && dir != &s_)
    {
        t_canvasenvironment *e = canvas_getenv(x);
        e->ce_dir = dir;
    }
}

/* --------------- traversing the set of lines in a canvas ----------- */

int canvas_getindex(t_canvas *x, t_gobj *y)
{
    t_gobj *y2;
    int indexno;
    for (indexno = 0, y2 = x->gl_list; y2 && y2 != y; y2 = y2->g_next)
        indexno++;
    return (indexno);
}

void linetraverser_start(t_linetraverser *t, t_canvas *x)
{
    t->tr_ob = 0;
    t->tr_x = x;
    t->tr_nextoc = 0;
    t->tr_nextoutno = t->tr_nout = 0;
}

t_outconnect *linetraverser_next(t_linetraverser *t)
{
    t_outconnect *rval = t->tr_nextoc;
    int outno;
    while (!rval)
    {
        outno = t->tr_nextoutno;
        while (outno == t->tr_nout)
        {
            t_gobj *y;
            t_object *ob = 0;
            if (!t->tr_ob) y = t->tr_x->gl_list;
            else y = t->tr_ob->ob_g.g_next;
            for (; y; y = y->g_next)
                if (ob = pd_checkobject(&y->g_pd)) break;
            if (!ob) return (0);
            t->tr_ob = ob;
            t->tr_nout = obj_noutlets(ob);
            outno = 0;
            if (glist_isvisible(t->tr_x))
                gobj_getrect(y, t->tr_x,
                    &t->tr_x11, &t->tr_y11, &t->tr_x12, &t->tr_y12);
            else t->tr_x11 = t->tr_y11 = t->tr_x12 = t->tr_y12 = 0;
        }
        t->tr_nextoutno = outno + 1;
        rval = obj_starttraverseoutlet(t->tr_ob, &t->tr_outlet, outno);
        t->tr_outno = outno;
    }
    t->tr_nextoc = obj_nexttraverseoutlet(rval, &t->tr_ob2,
        &t->tr_inlet, &t->tr_inno);
    t->tr_nin = obj_ninlets(t->tr_ob2);
    if (!t->tr_nin) bug("drawline");
    if (glist_isvisible(t->tr_x))
    {
        int inplus = (t->tr_nin == 1 ? 1 : t->tr_nin - 1);
        int outplus = (t->tr_nout == 1 ? 1 : t->tr_nout - 1);
        gobj_getrect(&t->tr_ob2->ob_g, t->tr_x,
            &t->tr_x21, &t->tr_y21, &t->tr_x22, &t->tr_y22);
        t->tr_lx1 = t->tr_x11 +
            ((t->tr_x12 - t->tr_x11 - IOWIDTH) * t->tr_outno) /
                outplus + IOMIDDLE;
        t->tr_ly1 = t->tr_y12;
        t->tr_lx2 = t->tr_x21 +
            ((t->tr_x22 - t->tr_x21 - IOWIDTH) * t->tr_inno)/inplus +
                IOMIDDLE;
        t->tr_ly2 = t->tr_y21;
    }
    else
    {
        t->tr_x21 = t->tr_y21 = t->tr_x22 = t->tr_y22 = 0;
        t->tr_lx1 = t->tr_ly1 = t->tr_lx2 = t->tr_ly2 = 0;
    }
    return (rval);
}

void linetraverser_skipobject(t_linetraverser *t)
{
    t->tr_nextoc = 0;
    t->tr_nextoutno = t->tr_nout;
}

/* -------------------- the canvas object -------------------------- */
int glist_valid = 10000;

void glist_init(t_glist *x)
{
        /* zero out everyone except "pd" field */
    memset(((char *)x) + sizeof(x->gl_pd), 0, sizeof(*x) - sizeof(x->gl_pd));
    x->gl_stub = gstub_new(x, 0);
    x->gl_valid = ++glist_valid;
    x->gl_xlabel = (t_symbol **)t_getbytes(0);
    x->gl_ylabel = (t_symbol **)t_getbytes(0);
}

    /* make a new glist.  It will either be a "root" canvas or else
    it appears as a "text" object in another window (canvas_getcurrent() 
    tells us which.) */
t_canvas *canvas_new(void *dummy, t_symbol *sel, int argc, t_atom *argv)
{
    t_canvas *x = (t_canvas *)pd_new(canvas_class);
    t_canvas *owner = canvas_getcurrent();
    t_symbol *s = &s_;
    int vis = 0, width = GLIST_DEFCANVASWIDTH, height = GLIST_DEFCANVASHEIGHT;
    int xloc = 0, yloc = GLIST_DEFCANVASYLOC;
    int font = (owner ? owner->gl_font : sys_defaultfont);

    glist_init(x);
    //x->gl_magic_glass = magicGlass_new(x);
    x->gl_obj.te_type = T_OBJECT;
    if (!owner)
        canvas_addtolist(x);
    /* post("canvas %lx, owner %lx", x, owner); */

    if (argc == 5)  /* toplevel: x, y, w, h, font */
    {
        xloc = atom_getintarg(0, argc, argv);
        yloc = atom_getintarg(1, argc, argv);
        width = atom_getintarg(2, argc, argv);
        height = atom_getintarg(3, argc, argv);
        font = atom_getintarg(4, argc, argv);
    }
    else if (argc == 6)  /* subwindow: x, y, w, h, name, vis */
    {
        xloc = atom_getintarg(0, argc, argv);
        yloc = atom_getintarg(1, argc, argv);
        width = atom_getintarg(2, argc, argv);
        height = atom_getintarg(3, argc, argv);
        s = atom_getsymbolarg(4, argc, argv);
        vis = atom_getintarg(5, argc, argv);
    }
        /* (otherwise assume we're being created from the menu.) */

    if (canvas_newdirectory->s_name[0])
    {
        static int dollarzero = 1000;
        t_canvasenvironment *env = x->gl_env =
            (t_canvasenvironment *)getbytes(sizeof(*x->gl_env));
        if (!canvas_newargv)
            canvas_newargv = getbytes(0);
        env->ce_dir = canvas_newdirectory;
        env->ce_argc = canvas_newargc;
        env->ce_argv = canvas_newargv;
        env->ce_dollarzero = dollarzero++;
        env->ce_path = 0;
        canvas_newdirectory = &s_;
        canvas_newargc = 0;
        canvas_newargv = 0;
    }
    else x->gl_env = 0;

    if (yloc < GLIST_DEFCANVASYLOC)
        yloc = GLIST_DEFCANVASYLOC;
    if (xloc < 0)
        xloc = 0;
    x->gl_x1 = 0;
    x->gl_y1 = 0;
    x->gl_x2 = 1;
    x->gl_y2 = 1;
    canvas_setbounds(x, xloc, yloc, xloc + width, yloc + height);
    x->gl_owner = owner;
    x->gl_name = (*s->s_name ? s : 
        (canvas_newfilename ? canvas_newfilename : gensym("Pd")));
    canvas_bind(x);
    x->gl_loading = 1;
	//fprintf(stderr,"loading = 1 .x%lx owner=.x%lx\n", (t_int)x, (t_int)x->gl_owner);
    x->gl_goprect = 0;      /* no GOP rectangle unless it's turned on later */
        /* cancel "vis" flag if we're a subpatch of an
         abstraction inside another patch.  A separate mechanism prevents
         the toplevel abstraction from showing up. */
    if (vis && gensym("#X")->s_thing && 
        ((*gensym("#X")->s_thing) == canvas_class))
    {
        t_canvas *zzz = (t_canvas *)(gensym("#X")->s_thing);
        while (zzz && !zzz->gl_env)
            zzz = zzz->gl_owner;
        if (zzz && canvas_isabstraction(zzz) && zzz->gl_owner)
            vis = 0;
    }
    x->gl_willvis = vis;
    x->gl_edit = !strncmp(x->gl_name->s_name, "Untitled", 8);
    x->gl_font = sys_nearestfontsize(font);
    pd_pushsym(&x->gl_pd);

	//dpsaha@vt.edu gop resize

	//resize blob	
	t_scalehandle *sh;
	char buf[64];
	x->x_handle = pd_new(scalehandle_class);
	sh = (t_scalehandle *)x->x_handle;
	sh->h_master = (t_gobj*)x;
	sprintf(buf, "_h%lx", (t_int)sh);
	pd_bind(x->x_handle, sh->h_bindsym = gensym(buf));
	sprintf(sh->h_outlinetag, "h%lx", (t_int)sh);
	sh->h_dragon = 0;
	sh->h_scale = 1;
	x->scale_offset_x = 0;
	x->scale_offset_y = 0;
	x->scale_vis = 0;
	
	//move blob	
	t_scalehandle *mh;
	char mbuf[64];
	x->x_mhandle = pd_new(scalehandle_class);
	mh = (t_scalehandle *)x->x_mhandle;
	mh->h_master = (t_gobj*)x;
	sprintf(mbuf, "_h%lx", (t_int)mh);
	pd_bind(x->x_mhandle, mh->h_bindsym = gensym(mbuf));
	sprintf(mh->h_outlinetag, "h%lx", (t_int)mh);
	mh->h_dragon = 0;
	mh->h_scale = 0;
	x->move_offset_x = 0;
	x->move_offset_y = 0;
	x->move_vis = 0;

	x->u_queue = canvas_undo_init(x);

    return(x);
}

void canvas_setgraph(t_glist *x, int flag, int nogoprect);

static void canvas_coords(t_glist *x, t_symbol *s, int argc, t_atom *argv)
{
	//IB: first delete the graph in case we are downsizing the object size via script
	canvas_setgraph(x, 0, 0);

    x->gl_x1 = atom_getfloatarg(0, argc, argv);
    x->gl_y1 = atom_getfloatarg(1, argc, argv);
    x->gl_x2 = atom_getfloatarg(2, argc, argv);
    x->gl_y2 = atom_getfloatarg(3, argc, argv);
    x->gl_pixwidth = atom_getintarg(4, argc, argv);
    x->gl_pixheight = atom_getintarg(5, argc, argv);
    if (argc <= 7)
        canvas_setgraph(x, atom_getintarg(6, argc, argv), 1);
    else
    {
        x->gl_xmargin = atom_getintarg(7, argc, argv);
        x->gl_ymargin = atom_getintarg(8, argc, argv);
        canvas_setgraph(x, atom_getintarg(6, argc, argv), 0);
    }
}

    /* make a new glist and add it to this glist.  It will appear as
    a "graph", not a text object.  */
t_glist *glist_addglist(t_glist *g, t_symbol *sym,
    t_float x1, t_float y1, t_float x2, t_float y2,
    t_float px1, t_float py1, t_float px2, t_float py2)
{
    static int gcount = 0;
    int zz;
    int menu = 0;
    char *str;
    t_glist *x = (t_glist *)pd_new(canvas_class);
    glist_init(x);
    x->gl_obj.te_type = T_OBJECT;
    if (!*sym->s_name)
    {
        char buf[40];
        sprintf(buf, "graph%d", ++gcount);
        sym = gensym(buf);
        menu = 1;
    }
    else if (!strncmp((str = sym->s_name), "graph", 5)
        && (zz = atoi(str + 5)) > gcount)
            gcount = zz;
        /* in 0.34 and earlier, the pixel rectangle and the y bounds were
        reversed; this would behave the same, except that the dialog window
        would be confusing.  The "correct" way is to have "py1" be the value
        that is higher on the screen. */
    if (py2 < py1)
    {
        t_float zz;
        zz = y2;
        y2 = y1;
        y1 = zz;
        zz = py2;
        py2 = py1;
        py1 = zz;
    }
    if (x1 == x2 || y1 == y2)
        x1 = 0, x2 = 100, y1 = 1, y2 = -1;
	if (px1 != 0 && px2 == 0) px2 = px1 + GLIST_DEFGRAPHWIDTH;
	if (py1 != 0 && py2 == py1) py2 = py1 + GLIST_DEFGRAPHHEIGHT;
    if (px1 >= px2 || py1 >= py2)
        px1 = 100, py1 = 20, px2 = 100 + GLIST_DEFGRAPHWIDTH,
            py2 = 20 + GLIST_DEFGRAPHHEIGHT;

    x->gl_name = sym;
    x->gl_x1 = x1;
    x->gl_x2 = x2;
    x->gl_y1 = y1;
    x->gl_y2 = y2;
    x->gl_obj.te_xpix = px1;
    x->gl_obj.te_ypix = py1;
    x->gl_pixwidth = px2 - px1;
    x->gl_pixheight = py2 - py1;
    x->gl_font =  (canvas_getcurrent() ?
        canvas_getcurrent()->gl_font : sys_defaultfont);
    x->gl_screenx1 = x->gl_screeny1 = 0;
    x->gl_screenx2 = 450;
    x->gl_screeny2 = 300;
    x->gl_owner = g;
	canvas_bind(x);
    x->gl_isgraph = 1;
    x->gl_goprect = 0;
    x->gl_obj.te_binbuf = binbuf_new();
    binbuf_addv(x->gl_obj.te_binbuf, "s", gensym("graph"));
    if (!menu)
        pd_pushsym(&x->gl_pd);
    glist_add(g, &x->gl_gobj);
		if (!do_not_redraw) sys_vgui("pdtk_canvas_getscroll .x%lx.c\n", (long unsigned int)glist_getcanvas(g));
    return (x);
}

    /* call glist_addglist from a Pd message */
void glist_glist(t_glist *g, t_symbol *s, int argc, t_atom *argv)
{
	pd_vmess(&g->gl_pd, gensym("editmode"), "i", 1);
    t_symbol *sym = atom_getsymbolarg(0, argc, argv);
	/* if we wish to put a graph where the mouse is we need to replace bogus name */
	if (!strcmp(sym->s_name, "NULL")) sym = &s_;  
    t_float x1 = atom_getfloatarg(1, argc, argv);  
    t_float y1 = atom_getfloatarg(2, argc, argv);  
    t_float x2 = atom_getfloatarg(3, argc, argv);  
    t_float y2 = atom_getfloatarg(4, argc, argv);  
    t_float px1 = atom_getfloatarg(5, argc, argv);  
    t_float py1 = atom_getfloatarg(6, argc, argv);  
    t_float px2 = atom_getfloatarg(7, argc, argv);  
    t_float py2 = atom_getfloatarg(8, argc, argv);
    glist_addglist(g, sym, x1, y1, x2, y2, px1, py1, px2, py2);
}

    /* return true if the glist should appear as a graph on parent;
    otherwise it appears as a text box. */
int glist_isgraph(t_glist *x)
{
    // testing to see if we have an array and force hiding text (later update GUI accordingly)
    // we likely need this to silently update legacy arrays
    // (no regressions are expected but this needs to be tested)
    t_gobj *g = x->gl_list;
    int hasarray = 0;
    while (g) {
        if (pd_class(&g->g_pd) == garray_class) hasarray = 1;
        g = g->g_next;
    }
    if (hasarray)  {
        x->gl_isgraph = 1;
        x->gl_hidetext = 1;
    }
    return (x->gl_isgraph|(x->gl_hidetext<<1));
}

    /* This is sent from the GUI to inform a toplevel that its window has been
    moved or resized. */
void canvas_setbounds(t_canvas *x, int x1, int y1, int x2, int y2)
{
	//fprintf(stderr,"canvas_setbounds %d %d %d %d\n", x1, y1, x2, y2);

    int heightwas = y2 - y1;
    int heightchange = y2 - y1 - (x->gl_screeny2 - x->gl_screeny1);
    if (x->gl_screenx1 == x1 && x->gl_screeny1 == y1 &&
        x->gl_screenx2 == x2 && x->gl_screeny2 == y2)
            return;
    x->gl_screenx1 = x1;
    x->gl_screeny1 = y1;
    x->gl_screenx2 = x2;
    x->gl_screeny2 = y2;
    if (!glist_isgraph(x) && (x->gl_y2 < x->gl_y1)) 
    {
            /* if it's flipped so that y grows upward,
            fix so that zero is bottom edge and redraw.  This is
            only appropriate if we're a regular "text" object on the
            parent. */
        t_float diff = x->gl_y1 - x->gl_y2;
        t_gobj *y;
        x->gl_y1 = heightwas * diff;
        x->gl_y2 = x->gl_y1 - diff;
            /* and move text objects accordingly; they should stick
            to the bottom, not the top. */
        for (y = x->gl_list; y; y = y->g_next)
            if (pd_checkobject(&y->g_pd))
                gobj_displace(y, x, 0, heightchange);
        canvas_redraw(x);
    }
}

t_symbol *canvas_makebindsym(t_symbol *s)
{
    char buf[MAXPDSTRING];
    strcpy(buf, "pd-");
    strcat(buf, s->s_name);
    return (gensym(buf));
}

void canvas_reflecttitle(t_canvas *x)
{
	//fprintf(stderr,"canvas_reflecttitle\n");
    char namebuf[MAXPDSTRING];
    t_canvasenvironment *env = canvas_getenv(x);
    if (env->ce_argc)
    {
        int i;
        strcpy(namebuf, " (");
        for (i = 0; i < env->ce_argc; i++)
        {
            if (strlen(namebuf) > MAXPDSTRING/2 - 5)
                break;
            if (i != 0)
                strcat(namebuf, " ");
            atom_string(&env->ce_argv[i], namebuf + strlen(namebuf), 
                MAXPDSTRING/2);
        }
        strcat(namebuf, ")");
    }
    else namebuf[0] = 0;
#ifdef __APPLE__
    sys_vgui("wm attributes .x%lx -modified %d -titlepath {%s/%s}\n",
        x, x->gl_dirty, canvas_getdir(x)->s_name, x->gl_name->s_name);
    sys_vgui("wm title .x%lx {%s%s}\n", x, x->gl_name->s_name, namebuf);
#else
	//if(glist_havewindow(x) || !x->gl_isgraph || x->gl_isgraph && x->gl_havewindow || x->gl_loading || x->gl_dirty) {
		/*fprintf(stderr,"%d %d %d %d %d\n", glist_istoplevel(x), !x->gl_isgraph,
			x->gl_isgraph && x->gl_havewindow, x->gl_loading,
			x->gl_dirty);*/
	sys_vgui("wm title .x%lx {%s%c%s - %s}\n", 
	        x, x->gl_name->s_name, (x->gl_dirty? '*' : ' '), namebuf,
            canvas_getdir(x)->s_name);
	//}
#endif
}

    /* mark a glist dirty or clean */
void canvas_dirty(t_canvas *x, t_floatarg n)
{
    t_canvas *x2 = canvas_getrootfor(x);
    if (glist_amreloadingabstractions)
        return;
    if ((unsigned)n != x2->gl_dirty)
    {
        x2->gl_dirty = n;
        if (glist_isvisible(x2))
            canvas_reflecttitle(x2);
    }
}

extern void canvas_check_nlet_highlights(t_canvas *x);

/*********** dpsaha@vt.edu resize move hooks ****************/
void canvas_draw_gop_resize_hooks(t_canvas* x)
{
	t_scalehandle *sh = (t_scalehandle *)(x->x_handle);
	t_scalehandle *mh = (t_scalehandle *)(x->x_mhandle);

    if (!sh || !mh) return; //in case we are an array which does not initialize its hooks

	if(x->gl_edit && glist_isvisible(x) && glist_istoplevel(x) && x->gl_goprect && !x->gl_editor->e_selection) {
		
		//Drawing and Binding Resize_Blob for GOP
		//fprintf(stderr,"draw_gop_resize_hooks %lx %lx\n", (t_int)x, (t_int)glist_getcanvas(x));
		sprintf(sh->h_pathname, ".x%lx.h%lx", (t_int)x, (t_int)sh);
		sys_vgui("destroy %s\n", sh->h_pathname);	
		sys_vgui(".x%lx.c delete GOP_resblob\n", x);	
		sys_vgui("canvas %s -width %d -height %d -bg $select_color -bd 0 -cursor bottom_right_corner\n",
				 sh->h_pathname, SCALEHANDLE_WIDTH, SCALEHANDLE_HEIGHT);
		sys_vgui(".x%x.c create window %d %d -anchor nw -width %d -height %d -window %s -tags {%lxSCALE %lxGOP GOP_resblob}\n",
				 x, x->gl_xmargin + x->gl_pixwidth - SCALEHANDLE_WIDTH - 1,
				 x->gl_ymargin + 3 + x->gl_pixheight - SCALEHANDLE_HEIGHT - 4,
				 SCALEHANDLE_WIDTH, SCALEHANDLE_HEIGHT,
				 sh->h_pathname, x, x);
		
		sys_vgui("bind %s <Button> {pd [concat %s _click 1 %%x %%y \\;]}\n",
				 sh->h_pathname, sh->h_bindsym->s_name);
		sys_vgui("bind %s <ButtonRelease> {pd [concat %s _click 0 0 0 \\;]}\n",
				 sh->h_pathname, sh->h_bindsym->s_name);
		sys_vgui("bind %s <Motion> {pd [concat %s _motion %%x %%y \\;]}\n",
				 sh->h_pathname, sh->h_bindsym->s_name);

		//Drawing and Binding Move_Blob for GOP
		sprintf(mh->h_pathname, ".x%lx.h%lx", (t_int)x, (t_int)mh);
		sys_vgui("destroy %s\n", mh->h_pathname);
		sys_vgui(".x%lx.c delete GOP_movblob\n", x);	
		sys_vgui("canvas %s -width %d -height %d -bg $select_color -bd 0 -cursor crosshair\n",
				 mh->h_pathname, SCALEHANDLE_WIDTH, SCALEHANDLE_HEIGHT);
		sys_vgui(".x%x.c create window %d %d -anchor nw -width %d -height %d -window %s -tags {%lxMOVE %lxGOP GOP_movblob}\n",
				 x, x->gl_xmargin + 2 ,
				 x->gl_ymargin + 2 ,
				 SCALEHANDLE_WIDTH, SCALEHANDLE_HEIGHT,
				 mh->h_pathname, x, x);
		
		sys_vgui("bind %s <Button> {pd [concat %s _click 1 %%x %%y \\;]}\n",
				 mh->h_pathname, mh->h_bindsym->s_name);
		sys_vgui("bind %s <ButtonRelease> {pd [concat %s _click 0 0 0 \\;]}\n",
				 mh->h_pathname, mh->h_bindsym->s_name);
		sys_vgui("bind %s <Motion> {pd [concat %s _motion %%x %%y \\;]}\n",
				 mh->h_pathname, mh->h_bindsym->s_name);

	}
	else{
		if (sh && sh->h_pathname)
			sys_vgui("destroy %s\n", sh->h_pathname);
		if (mh && mh->h_pathname)
			sys_vgui("destroy %s\n", mh->h_pathname);
		sys_vgui(".x%lx.c delete GOP_resblob ; .x%lx.c delete GOP_movblob ;\n", x, x);					//delete the GOP_resblob and GOP_movblob	
	}
	canvas_check_nlet_highlights(x);
}
/*****************************************************************************/

void canvas_drawredrect(t_canvas *x, int doit)
{
    if (doit){
        //fprintf(stderr,"GOP %d %d\n", x->gl_pixwidth, x->gl_pixheight);
        sys_vgui(".x%lx.c create line\
            %d %d %d %d %d %d %d %d %d %d -fill #ff8080 -tags GOP\n",
            glist_getcanvas(x),
            x->gl_xmargin, x->gl_ymargin,
            x->gl_xmargin + x->gl_pixwidth, x->gl_ymargin,
            x->gl_xmargin + x->gl_pixwidth, x->gl_ymargin + x->gl_pixheight,
            x->gl_xmargin, x->gl_ymargin + x->gl_pixheight,
            x->gl_xmargin, x->gl_ymargin);
		if (x->gl_goprect && x->gl_edit){
				canvas_draw_gop_resize_hooks(x);					//dpsaha@vt.edu for drawing the GOP_blobs
		}	
	}
    else sys_vgui(".x%lx.c delete GOP\n",  glist_getcanvas(x));
}

    /* the window becomes "mapped" (visible and not miniaturized) or
    "unmapped" (either miniaturized or just plain gone.)  This should be
    called from the GUI after the fact to "notify" us that we're mapped. */
void canvas_map(t_canvas *x, t_floatarg f)
{
	//fprintf(stderr,"canvas_map %lx %f\n", (t_int)x, f);
    int flag = (f != 0);
    t_gobj *y;
    if (flag)
    {
		//fprintf(stderr,"canvas_map 1\n");
        //if (!glist_isvisible(x))
        //{
			//fprintf(stderr,"canvas_map 1 isvisible\n");
        t_selection *sel;
        if (!x->gl_havewindow)
        {
            bug("canvas_map");
            canvas_vis(x, 1);
        }

		if (!x->gl_list) {
			//if there are no objects on the canvas
			canvas_create_editor(x);
		}
        else for (y = x->gl_list; y; y = y->g_next) {
            gobj_vis(y, x, 1);
		}
		if (x->gl_editor && x->gl_editor->e_selection)
        	for (sel = x->gl_editor->e_selection; sel; sel = sel->sel_next)
            	gobj_select(sel->sel_what, x, 1);
        x->gl_mapped = 1;
        canvas_drawlines(x);
        if (x->gl_isgraph && x->gl_goprect)
            canvas_drawredrect(x, 1);
        sys_vgui("pdtk_canvas_getscroll .x%lx.c\n", x);
        //}
    }
    else
    {
		//fprintf(stderr,"canvas_map 0\n");
        if (glist_isvisible(x))
        {
            /* just clear out the whole canvas */
			sys_vgui(".x%lx.c dtag all selected\n", x);
            //sys_vgui(".x%lx.c delete all\n", x);
			sys_vgui("foreach item [.x%lx.c find withtag {(!root)}] { .x%lx.c delete $item }\n", x, x);
            x->gl_mapped = 0;
        }
    }
}

void canvas_redraw(t_canvas *x)
{
	if (do_not_redraw) return;
	//fprintf(stderr,"canvas_redraw %lx\n", (t_int)x);
    if (glist_isvisible(x))
    {
		//fprintf(stderr,"canvas_redraw glist_isvisible=true\n");
        canvas_map(x, 0);
        canvas_map(x, 1);
		
		/* now re-highlight our selection */
	    t_selection *y;
		if (x->gl_editor && x->gl_editor->e_selection)
	    	for (y = x->gl_editor->e_selection; y; y = y->sel_next)		
				gobj_select(y->sel_what, x, 1);
    }
}


    /* we call this on a non-toplevel glist to "open" it into its
    own window. */
void glist_menu_open(t_glist *x)
{
    if (glist_isvisible(x))
    {
		if (!glist_istoplevel(x)) {
		    t_glist *gl2 = x->gl_owner;
		    if (!gl2) 
		        bug("glist_menu_open");  /* shouldn't happen but not dangerous */
		    else
		    {
		            /* erase ourself in parent window */
		        gobj_vis(&x->gl_gobj, gl2, 0);
		                /* get rid of our editor (and subeditors) */
		        if (x->gl_editor)
		            canvas_destroy_editor(x);
		        x->gl_havewindow = 1;
		                /* redraw ourself in parent window (blanked out this time) */
		        gobj_vis(&x->gl_gobj, gl2, 1);
		    }
		} else {
			sys_vgui("focus .x%lx\n", (t_int)x);
		}
    } else {
        if (x->gl_editor)
            canvas_destroy_editor(x);
	}
    canvas_vis(x, 1);
}

int glist_isvisible(t_glist *x)
{
    return ((!x->gl_loading) && glist_getcanvas(x)->gl_mapped);
}

int glist_istoplevel(t_glist *x)
{
        /* we consider a graph "toplevel" if it has its own window
        or if it appears as a box in its parent window so that we
        don't draw the actual contents there. */
    return (x->gl_havewindow || !x->gl_isgraph);
}

int glist_getfont(t_glist *x)
{
    while (!x->gl_env)
        if (!(x = x->gl_owner))
            bug("t_canvasenvironment");
    return (x->gl_font);
}

void canvas_free(t_canvas *x)
{
	//fprintf(stderr,"canvas_free %lx\n", x);
    t_gobj *y;
    int dspstate = canvas_suspend_dsp();

    //canvas_noundo(x);
	canvas_undo_free(x);

    if (canvas_editing == x)
        canvas_editing = 0;
    if (canvas_whichfind == x)
        canvas_whichfind = 0;
    glist_noselect(x);
    while (y = x->gl_list)
        glist_delete(x, y);
    if (x == glist_getcanvas(x))
        canvas_vis(x, 0);
     if (x->gl_editor)
         canvas_destroy_editor(x);   /* bug workaround; should already be gone*/
	canvas_unbind(x);
    if (x->gl_env)
    {
        freebytes(x->gl_env->ce_argv, x->gl_env->ce_argc * sizeof(t_atom));
        freebytes(x->gl_env, sizeof(*x->gl_env));
    }
    canvas_resume_dsp(dspstate);
    glist_cleanup(x);
    gfxstub_deleteforkey(x);        /* probably unnecessary */
    if (!x->gl_owner)
        canvas_takeofflist(x);
}

/* ----------------- lines ---------- */

static void canvas_drawlines(t_canvas *x)
{
    t_linetraverser t;
    t_outconnect *oc;
    int issignal;
    
        linetraverser_start(&t, x);
        while (oc = linetraverser_next(&t))
    {
        issignal = (outlet_getsymbol(t.tr_outlet) == &s_signal ? 1 : 0);
		if (!(pd_class(&t.tr_ob2->ob_g.g_pd) == preset_node_class && pd_class(&t.tr_ob->ob_g.g_pd) != message_class))
			canvas_drawconnection(glist_getcanvas(x), t.tr_lx1, t.tr_ly1, t.tr_lx2, t.tr_ly2, (t_int)oc, issignal);
		    /*sys_vgui(".x%lx.c create polyline %d %d %d %d -strokewidth %s -stroke %s \
	-tags {l%lx all_cords}\n",
		             glist_getcanvas(x), t.tr_lx1, t.tr_ly1, t.tr_lx2, t.tr_ly2, 
		             (issignal ? "$signal_cord_width" : "$msg_cord_width"), (issignal ? "$signal_cord" : "$msg_cord"),
		             oc);*/
    }
}

void canvas_fixlinesfor(t_canvas *x, t_text *text)
{
    t_linetraverser t;
    t_outconnect *oc;

    linetraverser_start(&t, x);
    while (oc = linetraverser_next(&t))
    {
        if (t.tr_ob == text || t.tr_ob2 == text)
        {
            /*sys_vgui(".x%lx.c coords l%lx %d %d %d %d\n",
                glist_getcanvas(x), oc,
                    t.tr_lx1, t.tr_ly1, t.tr_lx2, t.tr_ly2);*/
			canvas_updateconnection(x, t.tr_lx1, t.tr_ly1, t.tr_lx2, t.tr_ly2, (t_int)oc);
        }
    }
}

    /* kill all lines for the object */
void canvas_deletelinesfor(t_canvas *x, t_text *text)
{
    t_linetraverser t;
    t_outconnect *oc;
    linetraverser_start(&t, x);
    while (oc = linetraverser_next(&t))
    {
        if (t.tr_ob == text || t.tr_ob2 == text)
        {
            if (x->gl_editor && glist_isvisible(glist_getcanvas(x)))
            {
                sys_vgui(".x%lx.c delete l%lx\n",
                    glist_getcanvas(x), oc);
            }
            obj_disconnect(t.tr_ob, t.tr_outno, t.tr_ob2, t.tr_inno);
        }
    }
}

    /* 	delete all lines for the object 
		for efficient redrawing of connections */
void canvas_eraselinesfor(t_canvas *x, t_text *text)
{
    t_linetraverser t;
    t_outconnect *oc;
    linetraverser_start(&t, x);
    while (oc = linetraverser_next(&t))
    {
        if (t.tr_ob == text || t.tr_ob2 == text)
        {
            if (x->gl_editor)
            {
                sys_vgui(".x%lx.c delete l%lx\n",
                    glist_getcanvas(x), oc);
            }
        }
    }
}


    /* kill all lines for one inlet or outlet */
void canvas_deletelinesforio(t_canvas *x, t_text *text,
    t_inlet *inp, t_outlet *outp)
{
    t_linetraverser t;
    t_outconnect *oc;
    linetraverser_start(&t, x);
    while (oc = linetraverser_next(&t))
    {
        if ((t.tr_ob == text && t.tr_outlet == outp) ||
            (t.tr_ob2 == text && t.tr_inlet == inp))
        {
            if (x->gl_editor)
            {
                sys_vgui(".x%lx.c delete l%lx\n",
                    glist_getcanvas(x), oc);
            }
            obj_disconnect(t.tr_ob, t.tr_outno, t.tr_ob2, t.tr_inno);
        }
    }
}

static void canvas_pop(t_canvas *x, t_floatarg fvis)
{
    if (fvis != 0)
        canvas_vis(x, 1);
    pd_popsym(&x->gl_pd);
    canvas_resortinlets(x);
    canvas_resortoutlets(x);
    x->gl_loading = 0;
	sys_vgui("pdtk_canvas_force_getscroll .x%lx.c\n", x);
	//fprintf(stderr,"loading = 0 .x%lx owner=.x%lx\n", x, x->gl_owner);
}

void canvas_objfor(t_glist *gl, t_text *x, int argc, t_atom *argv);


void canvas_restore(t_canvas *x, t_symbol *s, int argc, t_atom *argv)
{
    t_pd *z;
    if (argc > 3)
    {
        t_atom *ap=argv+3;
        if (ap->a_type == A_SYMBOL)
        {
            t_canvasenvironment *e = canvas_getenv(canvas_getcurrent());
            canvas_rename(x, binbuf_realizedollsym(ap->a_w.w_symbol,
                e->ce_argc, e->ce_argv, 1), 0);
        }
    }
    canvas_pop(x, x->gl_willvis);

    if (!(z = gensym("#X")->s_thing)) error("canvas_restore: out of context");
    else if (*z != canvas_class) error("canvas_restore: wasn't a canvas");
    else
    {
        t_canvas *x2 = (t_canvas *)z;
        x->gl_owner = x2;
        canvas_objfor(x2, &x->gl_obj, argc, argv);
    }
}

void canvas_loadbangsubpatches(t_canvas *x, t_symbol *s)
{
    t_gobj *y;
    //t_symbol *s = gensym("loadbang");
    for (y = x->gl_list; y; y = y->g_next)
        if (pd_class(&y->g_pd) == canvas_class)
    {
        if (!canvas_isabstraction((t_canvas *)y)) {
			//fprintf(stderr,"%lx s:canvas_loadbangsubpatches %s\n",x,s->s_name);
            canvas_loadbangsubpatches((t_canvas *)y, s);
		}
    }
    for (y = x->gl_list; y; y = y->g_next)
        if ((pd_class(&y->g_pd) != canvas_class) &&
            zgetfn(&y->g_pd, s)) {
				//fprintf(stderr,"%lx s:obj_loadbang %s\n",x,s->s_name);
                pd_vmess(&y->g_pd, s, "");
		}
}

static void canvas_loadbangabstractions(t_canvas *x, t_symbol *s)
{
    t_gobj *y;
    //t_symbol *s = gensym("loadbang");
    for (y = x->gl_list; y; y = y->g_next)
        if (pd_class(&y->g_pd) == canvas_class)
    {
        if (canvas_isabstraction((t_canvas *)y)) {
			//fprintf(stderr,"%lx a:canvas_loadbang %s\n",x,s->s_name);
            canvas_loadbangabstractions((t_canvas *)y, s);
    		canvas_loadbangsubpatches((t_canvas *)y, s);
		}
        else {
			//fprintf(stderr,"%lx a:canvas_loadbangabstractions %s\n",x,s->s_name);
            canvas_loadbangabstractions((t_canvas *)y, s);
		}
    }
}

void canvas_loadbang(t_canvas *x)
{
    //t_gobj *y;
	// first loadbang preset hubs and nodes
	//fprintf(stderr,"%lx 0\n", x);
    canvas_loadbangabstractions(x, gensym("pre-loadbang"));
    canvas_loadbangsubpatches(x, gensym("pre-loadbang"));
	//fprintf(stderr,"%lx 1\n", x);
	// then do the regular loadbang
    canvas_loadbangabstractions(x, gensym("loadbang"));
    canvas_loadbangsubpatches(x, gensym("loadbang"));
	//fprintf(stderr,"%lx 2\n", x);
}

/* JMZ:
 * initbang is emitted after the canvas is done, but before the parent canvas is done
 * therefore, initbangs cannot reach to the outlets
 */
void canvas_initbang(t_canvas *x)
{
    t_gobj *y;
    t_symbol *s = gensym("initbang");
    /* run "initbang" for all subpatches, but NOT for the child abstractions */
    for (y = x->gl_list; y; y = y->g_next)
      if (pd_class(&y->g_pd) == canvas_class)
        {
          if (!canvas_isabstraction((t_canvas *)y))
            canvas_initbang((t_canvas *)y);
        }

    /* call the initbang()-method for objects that have one */
    for (y = x->gl_list; y; y = y->g_next)
      {
        if ((pd_class(&y->g_pd) != canvas_class) && zgetfn(&y->g_pd, s))
          {
            pd_vmess(&y->g_pd, s, "");
          }
      }
}
/* JMZ:
 * closebang is emitted before the canvas is destroyed
 * and BEFORE subpatches/abstractions in this canvas are destroyed
 */
void canvas_closebang(t_canvas *x)
{
    t_gobj *y;
    t_symbol *s = gensym("closebang");

    /* call the closebang()-method for objects that have one 
     * but NOT for subpatches/abstractions: these are called separately
     * from g_graph:glist_delete()
     */
    for (y = x->gl_list; y; y = y->g_next)
      {
        if ((pd_class(&y->g_pd) != canvas_class) && zgetfn(&y->g_pd, s))
          {
            pd_vmess(&y->g_pd, s, "");
          }
      }
}

/* needed for readjustment of garrays */
extern t_array *garray_getarray(t_garray *x);
extern void garray_fittograph(t_garray *x, int n);
extern t_rtext *glist_findrtext(t_glist *gl, t_text *who);
extern void rtext_gettext(t_rtext *x, char **buf, int *bufsize);

static void canvas_relocate(t_canvas *x, t_symbol *canvasgeom,
    t_symbol *topgeom)
{
    int cxpix, cypix, cw, ch, txpix, typix, tw, th;
    if (sscanf(canvasgeom->s_name, "%dx%d+%d+%d", &cw, &ch, &cxpix, &cypix)
        < 4 ||
        sscanf(topgeom->s_name, "%dx%d+%d+%d", &tw, &th, &txpix, &typix) < 4)
        bug("canvas_relocate");
            /* for some reason this is initially called with cw=ch=1 so
            we just suppress that here. */
    if (cw > 5 && ch > 5)
        canvas_setbounds(x, txpix, typix,
            txpix + cw, typix + ch);
	/* readjust garrays (if any) */
	t_gobj *g, *gg = NULL;
	t_garray *ga = NULL;
	t_array *a = NULL;
	int  num_elem = 0;

	for (g = x->gl_list; g; g = g->g_next) {
		//fprintf(stderr, "searching\n");

		//for subpatch garrays
		if (pd_class(&g->g_pd) == garray_class) {
			//fprintf(stderr,"found ya\n");
			ga = (t_garray *)g;
			if (ga) {
				a = garray_getarray(ga);
				num_elem = a->a_n;
				garray_fittograph(ga, num_elem);
			}
		}
	}
}

void canvas_popabstraction(t_canvas *x)
{
    newest = &x->gl_pd;
    pd_popsym(&x->gl_pd);
    //x->gl_loading = 1;
	//fprintf(stderr,"loading = 1 .x%lx owner=.x%lx\n", x, x->gl_owner);
    canvas_resortinlets(x);
    canvas_resortoutlets(x);
    x->gl_loading = 0;
	//fprintf(stderr,"loading = 0 .x%lx owner=.x%lx\n", x, x->gl_owner);
}

void canvas_logerror(t_object *y)
{
#ifdef LATER
    canvas_vis(x, 1);
    if (!glist_isselected(x, &y->ob_g))
        glist_select(x, &y->ob_g);
#endif
}

/* -------------------------- subcanvases ---------------------- */

static void *subcanvas_new(t_symbol *s)
{
    t_atom a[6];
    t_canvas *x, *z = canvas_getcurrent();
    fprintf(stderr,"subcanvas_new current canvas .x%lx\n", (t_int)z);
    if (!*s->s_name) s = gensym("/SUBPATCH/");
    SETFLOAT(a, 0);
    SETFLOAT(a+1, GLIST_DEFCANVASYLOC);
    SETFLOAT(a+2, GLIST_DEFCANVASWIDTH);
    SETFLOAT(a+3, GLIST_DEFCANVASHEIGHT);
    SETSYMBOL(a+4, s);
    SETFLOAT(a+5, 1);
    x = canvas_new(0, 0, 6, a);
    x->gl_owner = z;
    canvas_pop(x, 1);
    return (x);
}

static void canvas_click(t_canvas *x,
    t_floatarg xpos, t_floatarg ypos,
        t_floatarg shift, t_floatarg ctrl, t_floatarg alt)
{
    canvas_vis(x, 1);
}


    /* find out from subcanvas contents how much to fatten the box */
void canvas_fattensub(t_canvas *x,
    int *xp1, int *yp1, int *xp2, int *yp2)
{
    t_gobj *y;
    *xp2 += 50;     /* fake for now */
    *yp2 += 50;
}

static void canvas_rename_method(t_canvas *x, t_symbol *s, int ac, t_atom *av)
{
    if (ac && av->a_type == A_SYMBOL)
        canvas_rename(x, av->a_w.w_symbol, 0);
    else if (ac && av->a_type == A_DOLLSYM)
    {
        t_canvasenvironment *e = canvas_getenv(x);
        canvas_setcurrent(x);
        canvas_rename(x, binbuf_realizedollsym(av->a_w.w_symbol,
            e->ce_argc, e->ce_argv, 1), 0); 
        canvas_unsetcurrent(x);
    }
    else canvas_rename(x, gensym("Pd"), 0);
}

static int forwardmess_recurse = 0;

static void canvas_forwardmess(t_canvas *x, t_symbol *s, int ac, t_atom *av)
{
    if (av[0].a_type != A_FLOAT)
    {
        pd_error(x, "error: canvas: forwardmess: need object index");
        return;
    }
    t_int indexno = (t_int)atom_getfloatarg(0, ac--, av++);
    if (indexno < 0) indexno = 0;
    t_gobj *y;
    t_int i;
    for (i = 0, y = x->gl_list; y && i < indexno; i++)
        y = y->g_next;
    if (!y) return;
    if (forwardmess_recurse++ < 1)
        pd_forwardmess((t_pd *)y, ac, av);
    else
        pd_error(y, "error: canvas: forwardmess can't be in a recursive loop");
    forwardmess_recurse = 0;
}

/* ------------------ table ---------------------------*/

static int tabcount = 0;

static void *table_new(t_symbol *s, t_floatarg f)
{
    t_atom a[9];
    t_glist *gl;
    t_canvas *x, *z = canvas_getcurrent();
    if (s == &s_)
    {
         char  tabname[255];
         t_symbol *t = gensym("table"); 
         sprintf(tabname, "%s%d", t->s_name, tabcount++);
         s = gensym(tabname); 
    }
    if (f <= 1)
        f = 100;
    SETFLOAT(a, 0);
    SETFLOAT(a+1, GLIST_DEFCANVASYLOC);
    SETFLOAT(a+2, 600);
    SETFLOAT(a+3, 400);
    SETSYMBOL(a+4, s);
    SETFLOAT(a+5, 0);
    x = canvas_new(0, 0, 6, a);

    x->gl_owner = z;

        /* create a graph for the table */
    gl = glist_addglist((t_glist*)x, &s_, 0, -1, (f > 1 ? f-1 : 1), 1,
        50, 350, 550, 50);

    graph_array(gl, s, &s_float, f, 0);

    canvas_pop(x, 0); 

    return (x);
}

    /* return true if the "canvas" object is an abstraction (so we don't
    save its contents, fogr example.)  */
int canvas_isabstraction(t_canvas *x)
{
    return (x->gl_env != 0);
}

    /* return true if the "canvas" object should be bound to a name */
static int canvas_should_bind(t_canvas *x)
{
        /* FIXME should have a "backwards compatible" mode */
        /* not named "Pd" && (is top level || is subpatch) */
    return strcmp(x->gl_name->s_name, "Pd") && (!x->gl_owner || !x->gl_env);
}

static void canvas_bind(t_canvas *x)
{
    if (canvas_should_bind(x))
        pd_bind(&x->gl_pd, canvas_makebindsym(x->gl_name));
}

static void canvas_unbind(t_canvas *x)
{
    if (canvas_should_bind(x))
        pd_unbind(&x->gl_pd, canvas_makebindsym(x->gl_name));
}

    /* return true if the "canvas" object is a "table". */
int canvas_istable(t_canvas *x)
{
    t_atom *argv = (x->gl_obj.te_binbuf? binbuf_getvec(x->gl_obj.te_binbuf):0);
    int argc = (x->gl_obj.te_binbuf? binbuf_getnatom(x->gl_obj.te_binbuf) : 0);
    int istable = (argc && argv[0].a_type == A_SYMBOL &&
        argv[0].a_w.w_symbol == gensym("table"));
    return (istable);
}

    /* return true if the "canvas" object should be treated as a text
    object.  This is true for abstractions but also for "table"s... */
/* JMZ: add a flag to gop-abstractions to hide the title */
int canvas_showtext(t_canvas *x)
{
    t_atom *argv = (x->gl_obj.te_binbuf? binbuf_getvec(x->gl_obj.te_binbuf):0);
    int argc = (x->gl_obj.te_binbuf? binbuf_getnatom(x->gl_obj.te_binbuf) : 0);
    int isarray = (argc && argv[0].a_type == A_SYMBOL &&
        argv[0].a_w.w_symbol == gensym("graph"));
    if(x->gl_hidetext)
      return 0;
    else
      return (!isarray);
}

    /* get the document containing this canvas */
t_canvas *canvas_getrootfor(t_canvas *x)
{
    if ((!x->gl_owner) || canvas_isabstraction(x))
        return (x);
    else return (canvas_getrootfor(x->gl_owner));
}

/* ------------------------- DSP chain handling ------------------------- */

EXTERN_STRUCT _dspcontext;
#define t_dspcontext struct _dspcontext

void ugen_start(void);
void ugen_stop(void);

t_dspcontext *ugen_start_graph(int toplevel, t_signal **sp,
    int ninlets, int noutlets);
void ugen_add(t_dspcontext *dc, t_object *x);
void ugen_connect(t_dspcontext *dc, t_object *x1, int outno,
    t_object *x2, int inno);
void ugen_done_graph(t_dspcontext *dc);

    /* schedule one canvas for DSP.  This is called below for all "root"
    canvases, but is also called from the "dsp" method for sub-
    canvases, which are treated almost like any other tilde object.  */

static void canvas_dodsp(t_canvas *x, int toplevel, t_signal **sp)
{
    t_linetraverser t;
    t_outconnect *oc;
    t_gobj *y;
    t_object *ob;
    t_symbol *dspsym = gensym("dsp");
    t_dspcontext *dc;    

        /* create a new "DSP graph" object to use in sorting this canvas.
        If we aren't toplevel, there are already other dspcontexts around. */

    dc = ugen_start_graph(toplevel, sp,
        obj_nsiginlets(&x->gl_obj),
        obj_nsigoutlets(&x->gl_obj));

        /* find all the "dsp" boxes and add them to the graph */

	if (x->gl_editor) {
		ob = &x->gl_editor->gl_magic_glass->x_obj;
		if (ob && magicGlass_bound(x->gl_editor->gl_magic_glass)) {
			//fprintf(stderr,"adding cord inspector to dsp %d\n", magicGlass_bound(x->gl_magic_glass));
			ugen_add(dc, ob);  // this t_canvas could be an array, hence no gl_magic_glass
		}
	}
    
    for (y = x->gl_list; y; y = y->g_next)
        if ((ob = pd_checkobject(&y->g_pd)) && zgetfn(&y->g_pd, dspsym))
            ugen_add(dc, ob);

        /* ... and all dsp interconnections */
    linetraverser_start(&t, x);
    while (oc = linetraverser_next(&t))
        if (obj_issignaloutlet(t.tr_ob, t.tr_outno))
            ugen_connect(dc, t.tr_ob, t.tr_outno, t.tr_ob2, t.tr_inno);

        /* finally, sort them and add them to the DSP chain */
    ugen_done_graph(dc);
}

static void canvas_dsp(t_canvas *x, t_signal **sp)
{
    canvas_dodsp(x, 0, sp);
}

    /* this routine starts DSP for all root canvases. */
static void canvas_start_dsp(void)
{
    t_canvas *x;
    if (canvas_dspstate) ugen_stop();
    else sys_gui("pdtk_pd_dsp ON\n");
    ugen_start();
    
    for (x = canvas_list; x; x = x->gl_next)
        canvas_dodsp(x, 1, 0);
    
    canvas_dspstate = 1;
}

static void canvas_stop_dsp(void)
{
    if (canvas_dspstate)
    {
        ugen_stop();
        sys_gui("pdtk_pd_dsp OFF\n");
        canvas_dspstate = 0;
    }
}

    /* DSP can be suspended before, and resumed after, operations which
    might affect the DSP chain.  For example, we suspend before loading and
    resume afterward, so that DSP doesn't get resorted for every DSP object
    in the patch. */

int canvas_suspend_dsp(void)
{
    int rval = canvas_dspstate;
	//fprintf(stderr,"canvas_suspend_dsp %d\n", rval);
    if (rval) canvas_stop_dsp();
    return (rval);
}

void canvas_resume_dsp(int oldstate)
{
	//fprintf(stderr,"canvas_resume_dsp %d\n", oldstate);
    if (oldstate) canvas_start_dsp();
}

    /* this is equivalent to suspending and resuming in one step. */
void canvas_update_dsp(void)
{
    if (canvas_dspstate) canvas_start_dsp();
}

void glob_dsp(void *dummy, t_symbol *s, int argc, t_atom *argv)
{
    int newstate;
    if (argc)
    {
        newstate = atom_getintarg(0, argc, argv);
        if (newstate && !canvas_dspstate)
        {
            sys_set_audio_state(1);
            canvas_start_dsp();
        }
        else if (!newstate && canvas_dspstate)
        {
            canvas_stop_dsp();
            sys_set_audio_state(0);
        }
    }
    else post("dsp state %d", canvas_dspstate);
}

void *canvas_getblock(t_class *blockclass, t_canvas **canvasp)
{
    t_canvas *canvas = *canvasp;
    t_gobj *g;
    void *ret = 0;
    for (g = canvas->gl_list; g; g = g->g_next)
    {
        if (g->g_pd == blockclass)
            ret = g;
    }
    *canvasp = canvas->gl_owner;
    return(ret);
}
    
/******************* redrawing  data *********************/

    /* redraw all "scalars" (do this if a drawing command is changed.) 
    LATER we'll use the "template" information to select which ones we
    redraw.   Action = 0 for redraw, 1 for draw only, 2 for erase. */
static void glist_redrawall(t_glist *gl, int action)
{
	//fprintf(stderr,"glist_redrawall\n");
    t_gobj *g;
    int vis = glist_isvisible(gl);
    for (g = gl->gl_list; g; g = g->g_next)
    {
        t_class *cl;
        if (vis && g->g_pd == scalar_class)
        {
            if (action == 1)
            {
                if (glist_isvisible(gl))
                    gobj_vis(g, gl, 1);
            }
            else if (action == 2)
            {
                if (glist_isvisible(gl))
                    gobj_vis(g, gl, 0);
            }
            else scalar_redraw((t_scalar *)g, gl);
        }
        else if (g->g_pd == canvas_class)
            glist_redrawall((t_glist *)g, action);
    }
	if (glist_isselected(glist_getcanvas(gl), (t_gobj *)gl)) {
		sys_vgui("pdtk_select_all_gop_widgets .x%lx %lx %d\n", glist_getcanvas(gl), gl, 1);
	}
}

    /* public interface for above. */
void canvas_redrawallfortemplate(t_template *template, int action)
{
    t_canvas *x;
        /* find all root canvases */
    for (x = canvas_list; x; x = x->gl_next)
        glist_redrawall(x, action);
}

    /* find the template defined by a canvas, and redraw all elements
    for that */
void canvas_redrawallfortemplatecanvas(t_canvas *x, int action)
{
	//fprintf(stderr,"canvas_redrawallfortemplatecanvas\n");
    t_gobj *g;
    t_template *tmpl;
    t_symbol *s1 = gensym("struct");
    for (g = x->gl_list; g; g = g->g_next)
    {
        t_object *ob = pd_checkobject(&g->g_pd);
        t_atom *argv;
        if (!ob || ob->te_type != T_OBJECT ||
            binbuf_getnatom(ob->te_binbuf) < 2)
            continue;
        argv = binbuf_getvec(ob->te_binbuf);
        if (argv[0].a_type != A_SYMBOL || argv[1].a_type != A_SYMBOL
            || argv[0].a_w.w_symbol != s1)
                continue;
        tmpl = template_findbyname(argv[1].a_w.w_symbol);
        canvas_redrawallfortemplate(tmpl, action);
    }
    canvas_redrawallfortemplate(0, action);
}

/* ------------------------------- declare ------------------------ */

/* put "declare" objects in a patch to tell it about the environment in
which objects should be created in this canvas.  This includes directories to
search ("-path", "-stdpath") and object libraries to load
("-lib" and "-stdlib").  These must be set before the patch containing
the "declare" object is filled in with its contents; so when the patch is
saved,  we throw early messages to the canvas to set the environment
before any objects are created in it. */

static t_class *declare_class;
extern t_class *import_class;

typedef struct _declare
{
    t_object x_obj;
    t_canvas *x_canvas;
    int x_useme;
} t_declare;

static void *declare_new(t_symbol *s, int argc, t_atom *argv)
{
    t_declare *x = (t_declare *)pd_new(declare_class);
    x->x_useme = 1;
    x->x_canvas = canvas_getcurrent();
        /* LATER update environment and/or load libraries */
    return (x);
}

static void declare_free(t_declare *x)
{
    x->x_useme = 0;
        /* LATER update environment */
}

void canvas_savedeclarationsto(t_canvas *x, t_binbuf *b)
{
    t_gobj *y;

    for (y = x->gl_list; y; y = y->g_next)
    {
        if (pd_class(&y->g_pd) == declare_class)
        {
            binbuf_addv(b, "s", gensym("#X"));
            binbuf_addbinbuf(b, ((t_declare *)y)->x_obj.te_binbuf);
            binbuf_addv(b, ";");
        }
        else if (pd_class(&y->g_pd) == import_class)
        {
            int i, argc;
            t_atom *argv;
            binbuf_addv(b, "s", gensym("#X"));
            binbuf_addv(b, "s", gensym("declare"));
            argc = binbuf_getnatom(((t_object *)y)->te_binbuf) - 1;
            argv = binbuf_getvec(((t_object *)y)->te_binbuf) + 1;
            for(i = 0; i < argc; ++i)
            {
                binbuf_addv(b, "s", gensym("-lib"));
                binbuf_add(b, 1, argv + i);
            }
            binbuf_addv(b, ";");
        }
        else if (pd_class(&y->g_pd) == canvas_class)
            canvas_savedeclarationsto((t_canvas *)y, b);
    }
}

static void canvas_completepath(char *from, char *to, int bufsize)
{
    if (sys_isabsolutepath(from))
    {
        to[0] = '\0';
    }
    else
    {   // if not absolute path, append Pd lib dir
        strncpy(to, sys_libdir->s_name, bufsize-4);
        to[bufsize-3] = '\0';
        strcat(to, "/");
    }
    strncat(to, from, bufsize-strlen(to));
    to[bufsize-1] = '\0';
}

static void canvas_declare(t_canvas *x, t_symbol *s, int argc, t_atom *argv)
{
    int i;
    t_canvasenvironment *e = canvas_getenv(x);
#if 0
    startpost("declare:: %s", s->s_name);
    postatom(argc, argv);
    endpost();
#endif
    for (i = 0; i < argc; i++)
    {
        char strbuf[FILENAME_MAX];
        char *flag = atom_getsymbolarg(i, argc, argv)->s_name;
        if ((argc > i+1) && !strcmp(flag, "-path"))
        {
            e->ce_path = namelist_append(e->ce_path, 
                atom_getsymbolarg(i+1, argc, argv)->s_name, 0);
            i++;
        }
        else if ((argc > i+1) && !strcmp(flag, "-stdpath"))
        {
            canvas_completepath(atom_getsymbolarg(i+1, argc, argv)->s_name,
                strbuf, FILENAME_MAX);
            e->ce_path = namelist_append(e->ce_path, strbuf, 0);
            i++;
        }
        else if ((argc > i+1) && !strcmp(flag, "-lib"))
        {
            sys_load_lib(x, atom_getsymbolarg(i+1, argc, argv)->s_name);
            i++;
        }
        else if ((argc > i+1) && !strcmp(flag, "-stdlib"))
        {
            canvas_completepath(atom_getsymbolarg(i+1, argc, argv)->s_name,
                strbuf, FILENAME_MAX);
            sys_load_lib(0, strbuf);
            i++;
        }
        else post("declare: %s: unknown declaration", flag);
    }
}

    /* utility function to read a file, looking first down the canvas's search
    path (set with "declare" objects in the patch and recursively in calling
    patches), then down the system one.  The filename is the concatenation of
    "name" and "ext".  "Name" may be absolute, or may be relative with
    slashes.  If anything can be opened, the true directory
    ais put in the buffer dirresult (provided by caller), which should
    be "size" bytes.  The "nameresult" pointer will be set somewhere in
    the interior of "dirresult" and will give the file basename (with
    slashes trimmed).  If "bin" is set a 'binary' open is
    attempted, otherwise ASCII (this only matters on Microsoft.) 
    If "x" is zero, the file is sought in the directory "." or in the
    global path.*/

int canvas_open(t_canvas *x, const char *name, const char *ext,
    char *dirresult, char **nameresult, unsigned int size, int bin)
{
    t_namelist *nl, thislist;
    int fd = -1;
	int result = 0;
    t_canvas *y;
	char final_name[FILENAME_MAX];

		/* first check for @pd_extra (and later possibly others) and ~/ and replace */
	sys_expandpathelems(name, final_name);

        /* first check if "name" is absolute (and if so, try to open) */
    if (sys_open_absolute(final_name, ext, dirresult, nameresult, size, bin, &fd))
        return (fd);
    
        /* otherwise "name" is relative; start trying in directories named
        in this and parent environments */
    for (y = x; y; y = y->gl_owner)
        if (y->gl_env)
    {
        t_namelist *nl;
        t_canvas *x2 = x;
        char *dir;
        while (x2 && x2->gl_owner)
            x2 = x2->gl_owner;
        dir = (x2 ? canvas_getdir(x2)->s_name : ".");
        for (nl = y->gl_env->ce_path; nl; nl = nl->nl_next)
        {
            char realname[FILENAME_MAX];
            if (sys_isabsolutepath(nl->nl_string))
            {
                realname[0] = '\0';
            }
            else
            {   /* if not absolute path, append Pd lib dir */
                strncpy(realname, dir, FILENAME_MAX);
                realname[FILENAME_MAX-3] = 0;
                strcat(realname, "/");
            }
            strncat(realname, nl->nl_string, FILENAME_MAX-strlen(realname));
            realname[FILENAME_MAX-1] = 0;
            if ((fd = sys_trytoopenone(realname, final_name, ext,
                dirresult, nameresult, size, bin)) >= 0)
                    return (fd);
        }
    }
    result = open_via_path((x ? canvas_getdir(x)->s_name : "."), final_name, ext, dirresult, nameresult, size, bin);
	return(result);
}

static void canvas_f(t_canvas *x, t_symbol *s, int argc, t_atom *argv)
{
    static int warned;
    //fprintf(stderr,"canvas_f .x%lx\n", (t_int)x);
    t_canvas *xp = x; //parent window for a special case dealing with subpatches
    t_gobj *g, *g2;
    t_object *ob;
    if (argc > 1 && !warned)
    {
        post("** ignoring width or font settings from future Pd version **");
        warned = 1;
    }
    if (!x->gl_list) {
        if (x->gl_owner && !x->gl_isgraph) {
            // this means that we are a canvas that was just created
            // and that our width applies to our appearance on our parent
            xp = x->gl_owner;
            for (g = xp->gl_list; g != (t_gobj *)x; g = g->g_next) {
                //fprintf(stderr,".x%lx .x%lx\n", (t_int)g, (t_int)x);
                ;
            }
            //fprintf(stderr,"done %d\n", (g != NULL ? 1: 0));
        } else return;
    } else {
    for (g = x->gl_list; g2 = g->g_next; g = g2)
        ;
    }
    //fprintf(stderr,"is canvas_class? %d\n", (pd_class(&g->g_pd) == canvas_class ? 1:0));
    if ((ob = pd_checkobject(&g->g_pd)) || pd_class(&g->g_pd) == canvas_class)
    {
        //fprintf(stderr,"f received\n");
        ob->te_width = atom_getfloatarg(0, argc, argv);
        if (glist_isvisible(xp))
        {
            gobj_vis(g, xp, 0);
            gobj_vis(g, xp, 1);
        }
    }
}

void canvasgop_draw_move(t_canvas *x, int doit)
{
	//delete the earlier GOP window so that when dragging 
	//there is only one GOP window present on parent
	sys_vgui(".x%lx.c delete GOP\n",  x);
		
	//redraw the GOP
	canvas_setgraph(x, x->gl_isgraph+2*x->gl_hidetext, 0);
	canvas_dirty(x, 1);
	if (x->gl_havewindow) {
	    canvas_redraw(x);
	}
	//fprintf(stderr,"%d %d\n", (x->gl_owner ? 1:0), glist_isvisible(x->gl_owner));

	if (x->gl_owner && glist_isvisible(x->gl_owner)) {
		glist_noselect(x);
		//vmess(&x->gl_owner->gl_obj.te_pd, gensym("menu-open"), "");
	    gobj_vis(&x->gl_gobj, x->gl_owner, 0);
	    gobj_vis(&x->gl_gobj, x->gl_owner, 1);
		//canvas_redraw(x->gl_owner);
	}
	
	//update scrollbars when GOP potentially exceeds window size
	t_canvas *canvas=(t_canvas *)glist_getcanvas(x);
	
	//if gop is being disabled go one level up
	if (!x->gl_isgraph && x->gl_owner && glist_isvisible(x->gl_owner)) {
		canvas=canvas->gl_owner;
		//canvas_redraw(canvas);
	}
	sys_vgui("pdtk_canvas_getscroll .x%lx.c\n", (t_int)x);
	if (x->gl_owner && glist_isvisible(x->gl_owner))
		sys_vgui("pdtk_canvas_getscroll .x%lx.c\n", (t_int)x->gl_owner);
}

extern int gfxstub_haveproperties(void *key);
extern void canvas_canvas_setundo(t_canvas *x);
extern void graph_checkgop_rect(t_gobj *z, t_glist *glist,
    int *xp1, int *yp1, int *xp2, int *yp2);

void canvasgop__clickhook(t_scalehandle *sh, t_floatarg f, t_floatarg xxx, t_floatarg yyy)
{
	int x1=0, y1=0, x2=0, y2=0; //for getrect

	t_canvas *x = (t_canvas *)(sh->h_master);

 	if (xxx) x->scale_offset_x = xxx;
 	if (yyy) x->scale_offset_y = yyy;

    int newstate = (int)f;
    if (sh->h_dragon && newstate == 0)
    {
		/* done dragging */
		if(sh->h_scale)														//enter if resize_gop hook
		{
			/* first set up the undo apply */
			//canvas_canvas_setundo(x);
			canvas_undo_add(x, 8, "apply", canvas_undo_set_canvas(x));

			if (sh->h_dragx || sh->h_dragy) 
			{
				x->gl_pixwidth = x->gl_pixwidth + sh->h_dragx - x->scale_offset_x;
				if (x->gl_pixwidth < SCALE_GOP_MINWIDTH)
					x->gl_pixwidth = SCALE_GOP_MINWIDTH;
				x->gl_pixheight = x->gl_pixheight + sh->h_dragy - x->scale_offset_y;
				if (x->gl_pixheight < SCALE_GOP_MINHEIGHT)
					x->gl_pixheight = SCALE_GOP_MINHEIGHT;

				// check if the text is not hidden
				// if so make minimum width and height based retrieved from getrect
				if (!x->gl_hidetext)
				{
					if (x->gl_owner) {
						gobj_getrect((t_gobj*)x, x->gl_owner, &x1, &y1, &x2, &y2);
						if (x2-x1 > x->gl_pixwidth) x->gl_pixwidth = x2-x1;
						if (y2-y1 > x->gl_pixheight) x->gl_pixheight = y2-y1;
					} else {
						graph_checkgop_rect((t_gobj*)x, x, &x1, &y1, &x2, &y2);
						if (x2-x1 > x->gl_pixwidth) x->gl_pixwidth = x2-x1;
						if (y2-y1 > x->gl_pixheight) x->gl_pixheight = y2-y1;
					}
				}

				canvas_dirty(x, 1);
			}

			int properties = gfxstub_haveproperties((void *)x);

			if (properties) {
				sys_vgui(".gfxstub%lx.xrange.entry3 delete 0 end\n", properties);
				sys_vgui(".gfxstub%lx.xrange.entry3 insert 0 %d\n", properties, x->gl_pixwidth);
				sys_vgui(".gfxstub%lx.yrange.entry3 delete 0 end\n", properties);
				sys_vgui(".gfxstub%lx.yrange.entry3 insert 0 %d\n", properties, x->gl_pixheight);
			}

			if (glist_isvisible(x))
			{
				sys_vgui(".x%x.c delete %s\n", x, sh->h_outlinetag);
				canvasgop_draw_move(x,1);
				canvas_fixlinesfor(x, (t_text *)x);
				sys_vgui("pdtk_canvas_getscroll .x%lx.c\n", x);
			}
		}
		else 																//enter if move_gop hook
		{
			/* first set up the undo apply */
			//canvas_canvas_setundo(x);
			canvas_undo_add(x, 8, "apply", canvas_undo_set_canvas(x));

			if (sh->h_dragx || sh->h_dragy) 
			{
				x->gl_xmargin = x->gl_xmargin + sh->h_dragx - x->scale_offset_x;
				x->gl_ymargin = x->gl_ymargin + sh->h_dragy - x->scale_offset_y;
				
				canvas_dirty(x, 1);
			}

			int properties = gfxstub_haveproperties((void *)x);
			if (properties) {
				sys_vgui(".gfxstub%lx.xrange.entry4 delete 0 end\n", properties);
				sys_vgui(".gfxstub%lx.xrange.entry4 insert 0 %d\n", properties, x->gl_xmargin);
				sys_vgui(".gfxstub%lx.yrange.entry4 delete 0 end\n", properties);
				sys_vgui(".gfxstub%lx.yrange.entry4 insert 0 %d\n", properties, x->gl_ymargin);
			}
		
			if (glist_isvisible(x))
			{
				canvasgop_draw_move(x,1);
				canvas_fixlinesfor(x, (t_text *)x);
				sys_vgui("pdtk_canvas_getscroll .x%lx.c\n", x);
			}
		}
    }
	else if (!sh->h_dragon && newstate)
    {
		if(sh->h_scale)														//enter if resize_gop hook
		{
			
			sys_vgui("lower %s\n", sh->h_pathname);
			sys_vgui(".x%lx.c delete GOP \n",  x);							//delete GOP rect where it started from
			sys_vgui(".x%x.c create rectangle %d %d %d %d\
	 			-outline $select_color -width 1 -tags %s\n",\
				 x, x->gl_xmargin, x->gl_ymargin,\
					x->gl_xmargin + x->gl_pixwidth,\
					x->gl_ymargin + x->gl_pixheight, sh->h_outlinetag);

			sh->h_dragx = 0;
			sh->h_dragy = 0;
		}
		else																//enter if move_gop hook
		{
			sys_vgui("lower %s\n", sh->h_pathname);
			sys_vgui(".x%lx.c delete GOP_resblob \n",  x);					//delete GOP_resblob when moving the whole GOP

			sh->h_dragx = 0;
			sh->h_dragy = 0;
		}
    }

	sh->h_dragon = newstate;
}

void canvasgop__motionhook(t_scalehandle *sh,t_floatarg f1, t_floatarg f2)
{
	t_canvas *x = (t_canvas *)(sh->h_master);
	int dx = (int)f1, dy = (int)f2;
	int newx, newy;
	
	if (sh->h_dragon)
    {
		if(sh->h_scale)													//enter if resize_gop hook
		{			
			newx = x->gl_xmargin + x->gl_pixwidth - x->scale_offset_x + dx;
			newy = x->gl_ymargin + x->gl_pixheight - x->scale_offset_y + dy;

			if (newx < x->gl_xmargin + SCALE_GOP_MINWIDTH)
				newx = x->gl_xmargin + SCALE_GOP_MINWIDTH;
			if (newy < x->gl_ymargin + SCALE_GOP_MINHEIGHT)
				newy = x->gl_ymargin + SCALE_GOP_MINHEIGHT;

			sys_vgui(".x%x.c coords %s %d %d %d %d\n",
				 x, sh->h_outlinetag, x->gl_xmargin,
				 x->gl_ymargin, newx, newy);

			sh->h_dragx = dx;
			sh->h_dragy = dy;

			int properties = gfxstub_haveproperties((void *)x);
			if (properties) {
				int new_w = x->gl_pixwidth - x->scale_offset_x + sh->h_dragx;
				int new_h = x->gl_pixheight - x->scale_offset_y + sh->h_dragy;
				sys_vgui(".gfxstub%lx.xrange.entry3 delete 0 end\n", properties);
				sys_vgui(".gfxstub%lx.xrange.entry3 insert 0 %d\n", properties, new_w);
				sys_vgui(".gfxstub%lx.yrange.entry3 delete 0 end\n", properties);
				sys_vgui(".gfxstub%lx.yrange.entry3 insert 0 %d\n", properties, new_h);
			}
		}
		else															//enter if move_gop hook
		{
			newx = x->gl_xmargin - x->scale_offset_x + dx;
			newy = x->gl_ymargin - x->scale_offset_y + dy;
		
			int properties = gfxstub_haveproperties((void *)x);
			if (properties) {
				sys_vgui(".gfxstub%lx.xrange.entry4 delete 0 end\n", properties);
				sys_vgui(".gfxstub%lx.xrange.entry4 insert 0 %d\n", properties, newx);
				sys_vgui(".gfxstub%lx.yrange.entry4 delete 0 end\n", properties);
				sys_vgui(".gfxstub%lx.yrange.entry4 insert 0 %d\n", properties, newy);
			}

			sys_vgui(".x%x.c coords GOP %d %d %d %d %d %d %d %d %d %d\n",
						x, newx, newy, newx+x->gl_pixwidth, newy,
						newx+x->gl_pixwidth, newy+x->gl_pixheight,
						newx, newy+x->gl_pixheight,
						newx, newy);

			sh->h_dragx = dx;
			sh->h_dragy = dy;			
		}
    }
}
/*------------------------------------------------------------------------*/

/* ------------------------------- setup routine ------------------------ */

    /* why are some of these "glist" and others "canvas"? */
extern void glist_text(t_glist *x, t_symbol *s, int argc, t_atom *argv);
extern void canvas_obj(t_glist *gl, t_symbol *s, int argc, t_atom *argv);
extern void canvas_obj_abstraction_from_menu(t_glist *gl, t_symbol *s, int argc, t_atom *argv);
extern void canvas_bng(t_glist *gl, t_symbol *s, int argc, t_atom *argv);
extern void canvas_toggle(t_glist *gl, t_symbol *s, int argc, t_atom *argv);
extern void canvas_vslider(t_glist *gl, t_symbol *s, int argc, t_atom *argv);
extern void canvas_hslider(t_glist *gl, t_symbol *s, int argc, t_atom *argv);
extern void canvas_vdial(t_glist *gl, t_symbol *s, int argc, t_atom *argv);
    /* old version... */
extern void canvas_hdial(t_glist *gl, t_symbol *s, int argc, t_atom *argv);
extern void canvas_hdial(t_glist *gl, t_symbol *s, int argc, t_atom *argv);
    /* new version: */
extern void canvas_hradio(t_glist *gl, t_symbol *s, int argc, t_atom *argv);
extern void canvas_vradio(t_glist *gl, t_symbol *s, int argc, t_atom *argv);
extern void canvas_vumeter(t_glist *gl, t_symbol *s, int argc, t_atom *argv);
extern void canvas_mycnv(t_glist *gl, t_symbol *s, int argc, t_atom *argv);
extern void canvas_numbox(t_glist *gl, t_symbol *s, int argc, t_atom *argv);
extern void canvas_msg(t_glist *gl, t_symbol *s, int argc, t_atom *argv);
extern void canvas_floatatom(t_glist *gl, t_symbol *s, int argc, t_atom *argv);
extern void canvas_symbolatom(t_glist *gl, t_symbol *s, int argc, t_atom *argv);
extern void glist_scalar(t_glist *canvas, t_symbol *s, int argc, t_atom *argv);

void g_graph_setup(void);
void g_editor_setup(void);
void g_readwrite_setup(void);
extern void canvas_properties(t_gobj *z);

void g_canvas_setup(void)
{
        /* we prevent the user from typing "canvas" in an object box
        by sending 0 for a creator function. */
    canvas_class = class_new(gensym("canvas"), 0,
        (t_method)canvas_free, sizeof(t_canvas), CLASS_NOINLET, 0);
            /* here is the real creator function, invoked in patch files
            by sending the "canvas" message to #N, which is bound
            to pd_camvasmaker. */
    class_addmethod(pd_canvasmaker, (t_method)canvas_new, gensym("canvas"),
        A_GIMME, 0);
    class_addmethod(canvas_class, (t_method)canvas_restore,
        gensym("restore"), A_GIMME, 0);
    class_addmethod(canvas_class, (t_method)canvas_coords,
        gensym("coords"), A_GIMME, 0);

/* -------------------------- objects ----------------------------- */
    class_addmethod(canvas_class, (t_method)canvas_obj,
        gensym("obj"), A_GIMME, A_NULL);
    class_addmethod(canvas_class, (t_method)canvas_obj_abstraction_from_menu,
        gensym("obj_abstraction"), A_GIMME, A_NULL);
    class_addmethod(canvas_class, (t_method)canvas_msg,
        gensym("msg"), A_GIMME, A_NULL);
    class_addmethod(canvas_class, (t_method)canvas_floatatom,
        gensym("floatatom"), A_GIMME, A_NULL);
    class_addmethod(canvas_class, (t_method)canvas_symbolatom,
        gensym("symbolatom"), A_GIMME, A_NULL);
    class_addmethod(canvas_class, (t_method)glist_text,
        gensym("text"), A_GIMME, A_NULL);
    class_addmethod(canvas_class, (t_method)glist_glist, gensym("graph"),
        A_GIMME, A_NULL);
    class_addmethod(canvas_class, (t_method)glist_scalar,
        gensym("scalar"), A_GIMME, A_NULL);

/* -------------- IEMGUI: button, toggle, slider, etc.  ------------ */
    class_addmethod(canvas_class, (t_method)canvas_bng, gensym("bng"),
                    A_GIMME, A_NULL);
    class_addmethod(canvas_class, (t_method)canvas_toggle, gensym("toggle"),
                    A_GIMME, A_NULL);
    class_addmethod(canvas_class, (t_method)canvas_vslider, gensym("vslider"),
                    A_GIMME, A_NULL);
    class_addmethod(canvas_class, (t_method)canvas_hslider, gensym("hslider"),
                    A_GIMME, A_NULL);
    class_addmethod(canvas_class, (t_method)canvas_hdial, gensym("hdial"),
                    A_GIMME, A_NULL);
    class_addmethod(canvas_class, (t_method)canvas_vdial, gensym("vdial"),
                    A_GIMME, A_NULL);
    class_addmethod(canvas_class, (t_method)canvas_hradio, gensym("hradio"),
                    A_GIMME, A_NULL);
    class_addmethod(canvas_class, (t_method)canvas_vradio, gensym("vradio"),
                    A_GIMME, A_NULL);
    class_addmethod(canvas_class, (t_method)canvas_vumeter, gensym("vumeter"),
                    A_GIMME, A_NULL);
    class_addmethod(canvas_class, (t_method)canvas_mycnv, gensym("mycnv"),
                    A_GIMME, A_NULL);
    class_addmethod(canvas_class, (t_method)canvas_numbox, gensym("numbox"),
                    A_GIMME, A_NULL);

/* ------------------------ gui stuff --------------------------- */
    class_addmethod(canvas_class, (t_method)canvas_pop, gensym("pop"),
        A_DEFFLOAT, A_NULL);
    class_addmethod(canvas_class, (t_method)canvas_loadbang,
        gensym("loadbang"), A_NULL);
    class_addmethod(canvas_class, (t_method)canvas_relocate,
        gensym("relocate"), A_SYMBOL, A_SYMBOL, A_NULL);
    class_addmethod(canvas_class, (t_method)canvas_vis,
        gensym("vis"), A_FLOAT, A_NULL);
    class_addmethod(canvas_class, (t_method)glist_menu_open,
        gensym("menu-open"), A_NULL);
    class_addmethod(canvas_class, (t_method)canvas_map,
        gensym("map"), A_FLOAT, A_NULL);
    class_addmethod(canvas_class, (t_method)canvas_dirty,
        gensym("dirty"), A_FLOAT, A_NULL);
    class_setpropertiesfn(canvas_class, (t_propertiesfn)canvas_properties);

/* ---------------------- list handling ------------------------ */
    class_addmethod(canvas_class, (t_method)glist_clear, gensym("clear"),
        A_NULL);

/* ----- subcanvases, which you get by typing "pd" in a box ---- */
    class_addcreator((t_newmethod)subcanvas_new, gensym("pd"), A_DEFSYMBOL, 0);
    class_addcreator((t_newmethod)subcanvas_new, gensym("page"),  A_DEFSYMBOL, 0);

    class_addmethod(canvas_class, (t_method)canvas_click,
        gensym("click"), A_FLOAT, A_FLOAT, A_FLOAT, A_FLOAT, A_FLOAT, 0);
    class_addmethod(canvas_class, (t_method)canvas_dsp, gensym("dsp"), 0);
    class_addmethod(canvas_class, (t_method)canvas_rename_method,
        gensym("rename"), A_GIMME, 0);
    class_addmethod(canvas_class, (t_method)canvas_forwardmess,
        gensym("forwardmess"), A_GIMME, 0);

/*---------------------------- tables -- GG ------------------- */

    class_addcreator((t_newmethod)table_new, gensym("table"),
        A_DEFSYM, A_DEFFLOAT, 0);

/*---------------------------- declare ------------------- */
    declare_class = class_new(gensym("declare"), (t_newmethod)declare_new,
        (t_method)declare_free, sizeof(t_declare), CLASS_NOINLET, A_GIMME, 0);
    class_addmethod(canvas_class, (t_method)canvas_declare,
        gensym("declare"), A_GIMME, 0);

/*--------------- future message to set formatting  -------------- */
    class_addmethod(canvas_class, (t_method)canvas_f,
        gensym("f"), A_GIMME, 0);
/* -------------- setups from other files for canvas_class ---------------- */
    g_graph_setup();
    g_editor_setup();
    g_readwrite_setup();

/* -------------- dpsaha@vt.edu gop resize move-----------------------*/
	scalehandle_class = class_new(gensym("_scalehandle"), 0, 0,
				  sizeof(t_scalehandle), CLASS_PD, 0);
	class_addmethod(scalehandle_class, (t_method)canvasgop__clickhook,
		    gensym("_click"), A_FLOAT, A_FLOAT, A_FLOAT, 0);
    class_addmethod(scalehandle_class, (t_method)canvasgop__motionhook,
		    gensym("_motion"), A_FLOAT, A_FLOAT, 0);

}
