#include <u.h>
#include <libc.h>
#include <draw.h>
#include <thread.h>
#include <cursor.h>
#include <mouse.h>
#include <keyboard.h>
#include <frame.h>
#include <fcall.h>
#include <plumb.h>
#include "dat.h"
#include "fns.h"

/*
 *  WASHINGTON (AP) - The Food and Drug Administration warned
 * consumers Wednesday not to use ``Rio'' hair relaxer products
 * because they may cause severe hair loss or turn hair green....
 *    The FDA urged consumers who have experienced problems with Rio
 * to notify their local FDA office, local health department or the
 * company at 1‑800‑543‑3002.
 */

void		resize(void);
void		move(void);
void		delete(void);
void		hide(void);
void		tile(void);
void		unhide(int);
void		newtile(int);
Image*	sweep(void);
Image*	bandsize(Window*);
Image*	drag(Window*);
void		resized(void);
Channel	*exitchan;	/* chan(int) */
Channel	*winclosechan; /* chan(Window*); */
Channel	*kbdchan;	/* chan(char*); */
Rectangle	viewr;
int		threadrforkflag = 0;	/* should be RFENVG but that hides rio from plumber */
int	ctrldown;

void	mousethread(void*);
void	keyboardthread(void*);
void	winclosethread(void*);
void	deletethread(void*);
void	initcmd(void*);
Channel* initkbd(void);

char		*fontname;

enum
{
	New,
	Reshape,
	Move,
	Delete,
	Hide,
	Tile,
	Exit,
};

enum
{
	Cut,
	Paste,
	Snarf,
	Plumb,
	Look,
	Send,
	Scroll,
};

char		*menu2str[] = {
 [Cut]		"cut",
 [Paste]		"paste",
 [Snarf]		"snarf",
 [Plumb]		"plumb",
 [Look]		"look",
 [Send]		"send",
 [Scroll]		"scroll",
			nil
};

Menu menu2 =
{
	menu2str
};

int	Hidden = Exit+1;

char		*menu3str[100] = {
 [New]		"New",
 [Reshape]	"Resize",
 [Move]		"Move",
 [Delete]		"Delete",
 [Hide]		"Hide",
 [Tile]		"Tile",
 [Exit]		"Exit",
			nil
};

Menu menu3 =
{
	menu3str
};

char *rcargv[] = { "rc", "-i", nil };
char *kbdargv[] = { "rc", "-c", nil, nil };

int errorshouldabort = 0;

void
derror(Display*, char *errorstr)
{
	error(errorstr);
}

void
usage(void)
{
	fprint(2, "usage: rio [-b] [-f font] [-i initcmd] [-k kbdcmd] [-s]\n");
	exits("usage");
}

void
threadmain(int argc, char *argv[])
{
	char *initstr, *kbdin, *s;
	char buf[256];
	Image *i;
	Rectangle r;

	if(strstr(argv[0], ".out") == nil){
		menu3str[Exit] = nil;
		Hidden--;
	}
	initstr = nil;
	kbdin = nil;
	maxtab = 0;
	ARGBEGIN{
	case 'b':
		reverse = ~0xFF;
		break;
	case 'f':
		fontname = EARGF(usage());
		break;
	case 'i':
		initstr = EARGF(usage());
		break;
	case 'k':
		if(kbdin != nil)
			usage();
		kbdin = EARGF(usage());
		break;
	case 's':
		scrolling = TRUE;
		break;
	case 'D':
		debug++;
		break;
	default:
		usage();
	}ARGEND

	if(getwd(buf, sizeof buf) == nil)
		startdir = estrdup(".");
	else
		startdir = estrdup(buf);
	if(fontname == nil)
		fontname = getenv("font");
	s = getenv("tabstop");
	if(s != nil)
		maxtab = strtol(s, nil, 0);
	if(maxtab == 0)
		maxtab = 4;
	free(s);

	if(fontname){
		/* check font before barging ahead */
		if(access(fontname, 0) < 0){
			fprint(2, "rio: can't access %s: %r\n", fontname);
			exits("font open");
		}
		putenv("font", fontname);
	}

	snarffd = open("/dev/snarf", OREAD|OCEXEC);
	gotscreen = access("/dev/screen", AEXIST)==0;

	if(geninitdraw(nil, derror, nil, "rio", nil, Refnone) < 0){
		fprint(2, "rio: can't open display: %r\n");
		exits("display open");
	}
	iconinit();

	exitchan = chancreate(sizeof(int), 0);
	winclosechan = chancreate(sizeof(Window*), 0);
	deletechan = chancreate(sizeof(char*), 0);

	view = screen;
	viewr = view->r;
	mousectl = initmouse(nil, screen);
	if(mousectl == nil)
		error("can't find mouse");
	mouse = mousectl;
	kbdchan = initkbd();
	if(kbdchan == nil)
		error("can't find keyboard");
	wscreen = allocscreen(screen, background, 0);
	if(wscreen == nil)
		error("can't allocate screen");
	draw(view, viewr, background, nil, ZP);
	flushimage(display, 1);

	timerinit();
	threadcreate(keyboardthread, nil, STACK);
	threadcreate(mousethread, nil, STACK);
	threadcreate(winclosethread, nil, STACK);
	threadcreate(deletethread, nil, STACK);
	filsys = filsysinit(xfidinit());

	if(filsys == nil)
		fprint(2, "rio: can't create file system server: %r\n");
	else{
		errorshouldabort = 1;	/* suicide if there's trouble after this */
		if(initstr)
			proccreate(initcmd, initstr, STACK);
		if(kbdin){
			kbdargv[2] = kbdin;
			r = screen->r;
			r.min.y = r.max.y-Dy(r)/3;
			i = allocwindow(wscreen, r, Refbackup, DNofill);
			wkeyboard = new(i, FALSE, scrolling, 0, nil, "/bin/rc", kbdargv);
			if(wkeyboard == nil)
				error("can't create keyboard window");
		}
		threadnotify(shutdown, 1);
		recv(exitchan, nil);
	}
	killprocs();
	threadexitsall(nil);
}

/*
 * /dev/snarf updates when the file is closed, so we must open our own
 * fd here rather than use snarffd
 */
void
putsnarf(void)
{
	int fd, i, n;

	if(snarffd<0 || nsnarf==0)
		return;
	fd = open("/dev/snarf", OWRITE);
	if(fd < 0)
		return;
	/* snarf buffer could be huge, so fprint will truncate; do it in blocks */
	for(i=0; i<nsnarf; i+=n){
		n = nsnarf-i;
		if(n >= 256)
			n = 256;
		if(fprint(fd, "%.*S", n, snarf+i) < 0)
			break;
	}
	close(fd);
}

void
getsnarf(void)
{
	int i, n, nb, nulls;
	char *s, *sn;

	if(snarffd < 0)
		return;
	sn = nil;
	i = 0;
	seek(snarffd, 0, 0);
	for(;;){
		if(i > MAXSNARF)
			break;
		if((s = realloc(sn, i+1024+1)) == nil)
			break;
		sn = s;
		if((n = read(snarffd, sn+i, 1024)) <= 0)
			break;
		i += n;
	}
	if(i == 0)
		return;
	sn[i] = 0;
	if((snarf = runerealloc(snarf, i+1)) != nil)
		cvttorunes(sn, i, snarf, &nb, &nsnarf, &nulls);
	free(sn);
}

void
initcmd(void *arg)
{
	char *cmd;

	cmd = arg;
	rfork(RFENVG|RFFDG|RFNOTEG|RFNAMEG);
	procexecl(nil, "/bin/rc", "rc", "-c", cmd, nil);
	fprint(2, "rio: exec failed: %r\n");
	exits("exec");
}

char *oknotes[] =
{
	"delete",
	"hangup",
	"kill",
	"exit",
	nil
};

int
shutdown(void *, char *msg)
{
	int i;
	static Lock shutdownlk;
	
	killprocs();
	for(i=0; oknotes[i]; i++)
		if(strncmp(oknotes[i], msg, strlen(oknotes[i])) == 0){
			lock(&shutdownlk);	/* only one can threadexitsall */
			threadexitsall(msg);
		}
	fprint(2, "rio %d: abort: %s\n", getpid(), msg);
	abort();
	exits(msg);
	return 0;
}

void
killprocs(void)
{
	int i;

	for(i=0; i<nwindow; i++)
		if(window[i]->notefd >= 0)
			write(window[i]->notefd, "hangup", 6); 
}

void movewindowforward(void) { 
  Window *w;
  int i;
  for(i = 0; i <= nwindow; i++) {
    if(window[i] == input) {
      if (i >= nwindow - 1) {
        w = window[0];
        incref(w);
        wtopme(w);
        wcurrent(w);
        wclose(w);
      } else {
        w = window[i+1];
        incref(w);
        wtopme(w);
        wcurrent(w);
        wclose(w);
      }
    }
  }
}

void movewindowbackwards(void) {
  Window *w;
  int i;
  for(i = 0; i <= nwindow; i++) {
    if(window[i] == input) {
      if (i == 0 ) {
        w = window[nwindow - 1];
        incref(w);
        wtopme(w);
        wcurrent(w);
        wclose(w);
      } else {
        w = window[i-1];
        incref(w);
        wtopme(w);
        wcurrent(w);
        wclose(w);
      }
    }
  }
}

void
keyboardthread(void*)
{
	char *s;

	threadsetname("keyboardthread");

	while(s = recvp(kbdchan)){
		if(*s == 'k' || *s == 'K')
			shiftdown = utfrune(s+1, Kshift) != nil;
		    ctrldown = utfrune(s+1, Kctl) != nil;
			// check if we've hit ctrl-shift-j
			if(shiftdown && s[1] == (char)0x4a) {
				movewindowforward();
			} else if (shiftdown && s[1] == (char)0x4b) {
				movewindowbackwards();
			} else if (shiftdown && s[1] == (char)0x4c) {
				Image *i;
				Rectangle r;
				r = screen->r;
				r.max.x = r.min.x+300;
				r.max.y = r.min.y+80;
				i = allocwindow(wscreen, r, Refbackup, DNofill);
				new(i, FALSE, scrolling, 0, nil, "/bin/rc", nil);
				tile();
			} else if (shiftdown && s[1] == (char)0x20 && s[2] == (char)0x0) {
				tile();
			} else {
				if(input == nil || sendp(input->ck, s) <= 0)
					free(s);
			}
	}
}

int
inborder(Rectangle r, Point xy)
{
	return ptinrect(xy, r) && !ptinrect(xy, insetrect(r, Selborder));
}

Rectangle
whichrect(Rectangle r, Point p, int which)
{
	switch(which){
	case 0:	/* top left */
		r = Rect(p.x, p.y, r.max.x, r.max.y);
		break;
	case 2:	/* top right */
		r = Rect(r.min.x, p.y, p.x+1, r.max.y);
		break;
	case 6:	/* bottom left */
		r = Rect(p.x, r.min.y, r.max.x, p.y+1);
		break;
	case 8:	/* bottom right */
		r = Rect(r.min.x, r.min.y, p.x+1, p.y+1);
		break;
	case 1:	/* top edge */
		r = Rect(r.min.x, p.y, r.max.x, r.max.y);
		break;
	case 5:	/* right edge */
		r = Rect(r.min.x, r.min.y, p.x+1, r.max.y);
		break;
	case 7:	/* bottom edge */
		r = Rect(r.min.x, r.min.y, r.max.x, p.y+1);
		break;
	case 3:	/* left edge */
		r = Rect(p.x, r.min.y, r.max.x, r.max.y);
		break;
	}
	return canonrect(r);
}

int
portion(int x, int lo, int hi)
{
	x -= lo;
	hi -= lo;
	if(hi < 20)
		return x > 0 ? 2 : 0;
	if(x < 20)
		return 0;
	if(x > hi-20)
		return 2;
	return 1;
}

int
whichcorner(Rectangle r, Point p)
{
	int i, j;
	
	i = portion(p.x, r.min.x, r.max.x);
	j = portion(p.y, r.min.y, r.max.y);
	return 3*j+i;
}

/* thread to allow fsysproc to synchronize window closing with main proc */
void
winclosethread(void*)
{
	Window *w;

	threadsetname("winclosethread");
	for(;;){
		w = recvp(winclosechan);
		wclose(w);
	}
}

/* thread to make Deleted windows that the client still holds disappear offscreen after an interval */
void
deletethread(void*)
{
	char *s;
	Image *i;

	threadsetname("deletethread");
	for(;;){
		s = recvp(deletechan);
		i = namedimage(display, s);
		if(i != nil){
			/* move it off-screen to hide it, since client is slow in letting it go */
			originwindow(i, i->r.min, view->r.max);
			freeimage(i);
			flushimage(display, 1);
		}
		free(s);
	}
}

void
deletetimeoutproc(void *v)
{
	char *s;

	s = v;
	sleep(750);	/* remove window from screen after 3/4 of a second */
	sendp(deletechan, s);
}

/*
 * Button 6 - keyboard toggle - has been pressed.
 * Send event to keyboard, wait for button up, send that.
 * Note: there is no coordinate translation done here; this
 * is just about getting button 6 to the keyboard simulator.
 */
void
keyboardhide(void)
{
	send(wkeyboard->mc.c, mouse);
	do
		readmouse(mousectl);
	while(mouse->buttons & (1<<5));
	send(wkeyboard->mc.c, mouse);
}

void
mousethread(void*)
{
	int sending, inside, scrolling, moving;
	Window *w, *winput;
	Image *i;
	Point xy;
	Mouse tmp;
	enum {
		MReshape,
		MMouse,
		NALT
	};
	static Alt alts[NALT+1];

	threadsetname("mousethread");
	sending = FALSE;
	scrolling = FALSE;

	alts[MReshape].c = mousectl->resizec;
	alts[MReshape].v = nil;
	alts[MReshape].op = CHANRCV;
	alts[MMouse].c = mousectl->c;
	alts[MMouse].v = &mousectl->Mouse;
	alts[MMouse].op = CHANRCV;
	alts[NALT].op = CHANEND;

	for(;;)
	    switch(alt(alts)){
		case MReshape:
			resized();
			break;
		case MMouse:
			if(wkeyboard!=nil && (mouse->buttons & (1<<5))){
				keyboardhide();
				break;
			}
		Again:
			moving = FALSE;
			winput = input;
			/* override everything for the keyboard window */
			if(wkeyboard!=nil && ptinrect(mouse->xy, wkeyboard->screenr)){
				/* make sure it's on top; this call is free if it is */
				wtopme(wkeyboard);
				winput = wkeyboard;
			}
			if(winput!=nil && !winput->deleted && winput->i!=nil){
				/* convert to logical coordinates */
				xy.x = mouse->xy.x + (winput->i->r.min.x-winput->screenr.min.x);
				xy.y = mouse->xy.y + (winput->i->r.min.y-winput->screenr.min.y);

				/* the up and down scroll buttons are not subject to the usual rules */
				if((mouse->buttons&(8|16)) && !winput->mouseopen)
					goto Sending;

				inside = ptinrect(mouse->xy, insetrect(winput->screenr, Selborder));
				if(winput->mouseopen)
					scrolling = FALSE;
				else if(scrolling)
					scrolling = mouse->buttons;
				else
					scrolling = mouse->buttons && ptinrect(xy, winput->scrollr);
				/* topped will be zero or less if window has been bottomed */
				if(sending == FALSE && !scrolling && inborder(winput->screenr, mouse->xy) && winput->topped>0)
					moving = TRUE;
				else if(inside && (scrolling || winput->mouseopen || (mouse->buttons&1)))
					sending = TRUE;
			}else
				sending = FALSE;
			if(sending){
			Sending:
				wsetcursor(winput, FALSE);
				if(mouse->buttons == 0)
					sending = FALSE;
				tmp = mousectl->Mouse;
				tmp.xy = xy;
				send(winput->mc.c, &tmp);
				continue;
			}
			if(moving && (mouse->buttons&7)){
				incref(winput);
				sweeping = TRUE;
				if(mouse->buttons & 3)
					i = bandsize(winput);
				else
					i = drag(winput);
				sweeping = FALSE;
				if(i != nil)
					wsendctlmesg(winput, Reshaped, i->r, i);
				wclose(winput);
				continue;
			}
			w = wpointto(mouse->xy);
			if(w!=nil && inborder(w->screenr, mouse->xy))
				riosetcursor(corners[whichcorner(w->screenr, mouse->xy)]);
			else
				wsetcursor(w, FALSE);
			/* we're not sending the event, but if button is down maybe we should */
			if(mouse->buttons){
				/* w->topped will be zero or less if window has been bottomed */
				if(w==nil || (w==winput && w->topped>0)){
					if(mouse->buttons & 1){
						;
					}else if(mouse->buttons & 2){
						if(winput && !winput->deleted && !winput->mouseopen){
							incref(winput);
							button2menu(winput);
							wclose(winput);
						}
					}else if(mouse->buttons & 4)
						button3menu();
				}else{
					/* if button 1 event in the window, top the window and wait for button up. */
					/* otherwise, top the window and pass the event on */
					if(wtop(mouse->xy) && (mouse->buttons!=1 || inborder(w->screenr, mouse->xy)))
						goto Again;
					goto Drain;
				}
			}
			break;

		Drain:
			do
				readmouse(mousectl);
			while(mousectl->buttons);
			goto Again;	/* recalculate mouse position, cursor */
		}
}

int
wtopcmp(void *a, void *b)
{
	return (*(Window**)a)->topped - (*(Window**)b)->topped;
}

void
resized(void)
{
	Image *im;
	int i, j;
	Rectangle r;
	Point o, n;
	Window *w;

	if(getwindow(display, Refnone) < 0)
		error("failed to re-attach window");
	freescrtemps();
	view = screen;
	freescreen(wscreen);
	wscreen = allocscreen(screen, background, 0);
	if(wscreen == nil)
		error("can't re-allocate screen");
	draw(view, view->r, background, nil, ZP);
	o = subpt(viewr.max, viewr.min);
	n = subpt(view->clipr.max, view->clipr.min);
	qsort(window, nwindow, sizeof(window[0]), wtopcmp);
	for(i=0; i<nwindow; i++){
		w = window[i];
		r = rectsubpt(w->i->r, viewr.min);
		r.min.x = (r.min.x*n.x)/o.x;
		r.min.y = (r.min.y*n.y)/o.y;
		r.max.x = (r.max.x*n.x)/o.x;
		r.max.y = (r.max.y*n.y)/o.y;
		r = rectaddpt(r, view->clipr.min);
		if(!goodrect(r))
			r = rectsubpt(w->i->r, subpt(w->i->r.min, r.min));
		for(j=0; j<nhidden; j++)
			if(w == hidden[j])
				break;
		incref(w);
		if(j < nhidden){
			im = allocimage(display, r, screen->chan, 0, DNofill);
			r = ZR;
		} else
			im = allocwindow(wscreen, r, Refbackup, DNofill);
		if(im)
			wsendctlmesg(w, Reshaped, r, im);
		wclose(w);
	}
	viewr = view->r;
	flushimage(display, 1);
}

int
obscured(Window *w, Rectangle r, int i)
{
	Window *t;

	if(Dx(r) < font->height || Dy(r) < font->height)
		return 1;
	if(!rectclip(&r, screen->r))
		return 1;
	for(; i<nwindow; i++){
		t = window[i];
		if(t == w || t->topped <= w->topped)
			continue;
		if(Dx(t->screenr) == 0 || Dy(t->screenr) == 0 || rectXrect(r, t->screenr) == 0)
			continue;
		if(r.min.y < t->screenr.min.y)
			if(!obscured(w, Rect(r.min.x, r.min.y, r.max.x, t->screenr.min.y), i))
				return 0;
		if(r.min.x < t->screenr.min.x)
			if(!obscured(w, Rect(r.min.x, r.min.y, t->screenr.min.x, r.max.y), i))
				return 0;
		if(r.max.y > t->screenr.max.y)
			if(!obscured(w, Rect(r.min.x, t->screenr.max.y, r.max.x, r.max.y), i))
				return 0;
		if(r.max.x > t->screenr.max.x)
			if(!obscured(w, Rect(t->screenr.max.x, r.min.y, r.max.x, r.max.y), i))
				return 0;
		return 1;
	}
	return 0;
}

static char*
shortlabel(char *s)
{
	enum { NBUF=60 };
	static char buf[NBUF*UTFmax];
	int i, k, l;
	Rune r;

	l = utflen(s);
	if(l < NBUF-2)
		return estrdup(s);
	k = i = 0;
	while(i < NBUF/2){
		k += chartorune(&r, s+k);
		i++;
	}
	strncpy(buf, s, k);
	strcpy(buf+k, "...");
	while((l-i) >= NBUF/2-4){
		k += chartorune(&r, s+k);
		i++;
	}
	strcat(buf, s+k);
	return estrdup(buf);
}

void
button3menu(void)
{
	int i, j, n;

	n = nhidden;
	for(i=0; i<nwindow; i++){
		for(j=0; j<n; j++)
			if(window[i] == hidden[j])
				break;
		if(j == n)
			if(obscured(window[i], window[i]->screenr, 0)){
				hidden[n++] = window[i];
				if(n >= nelem(hidden))
					break;
			}
	}
	if(n >= nelem(menu3str)-Hidden)
		n = nelem(menu3str)-Hidden-1;
	for(i=0; i<n; i++){
		free(menu3str[i+Hidden]);
		menu3str[i+Hidden] = shortlabel(hidden[i]->label);
	}
	for(i+=Hidden; menu3str[i]; i++){
		free(menu3str[i]);
		menu3str[i] = nil;
	}
	sweeping = TRUE;
	switch(i = menuhit(3, mousectl, &menu3, wscreen)){
	case -1:
		break;
	case New:
		new(sweep(), FALSE, scrolling, 0, nil, "/bin/rc", nil);
		break;
	case Reshape:
		resize();
		break;
	case Move:
		move();
		break;
	case Delete:
		delete();
		break;
	case Hide:
		hide();
		break;
	case Tile:
		tile();
		break;
	case Exit:
		if(Hidden > Exit){
			send(exitchan, nil);
			break;
		}
		/* else fall through */
	default:
		unhide(i);
		break;
	}
	sweeping = FALSE;
}

void
button2menu(Window *w)
{
	if(w->scrolling)
		menu2str[Scroll] = "noscroll";
	else
		menu2str[Scroll] = "scroll";
	switch(menuhit(2, mousectl, &menu2, wscreen)){
	case Cut:
		wsnarf(w);
		wcut(w);
		wscrdraw(w);
		break;

	case Snarf:
		wsnarf(w);
		break;

	case Paste:
		getsnarf();
		wpaste(w);
		wscrdraw(w);
		break;

	case Plumb:
		wplumb(w);
		break;

	case Look:
		wlook(w);
		break;

	case Send:
		getsnarf();
		wsnarf(w);
		if(nsnarf == 0)
			break;
		if(w->rawing){
			waddraw(w, snarf, nsnarf);
			if(snarf[nsnarf-1]!='\n' && snarf[nsnarf-1]!='\004')
				waddraw(w, L"\n", 1);
		}else{
			winsert(w, snarf, nsnarf, w->nr);
			if(snarf[nsnarf-1]!='\n' && snarf[nsnarf-1]!='\004')
				winsert(w, L"\n", 1, w->nr);
		}
		wsetselect(w, w->nr, w->nr);
		wshow(w, w->nr);
		break;

	case Scroll:
		if(w->scrolling ^= 1)
			wshow(w, w->nr);
		break;
	}
	flushimage(display, 1);
	wsendctlmesg(w, Wakeup, ZR, nil);
}

Point
onscreen(Point p)
{
	p.x = max(screen->clipr.min.x, p.x);
	p.x = min(screen->clipr.max.x-1, p.x);
	p.y = max(screen->clipr.min.y, p.y);
	p.y = min(screen->clipr.max.y-1, p.y);
	return p;
}

Image*
sweep(void)
{
	Image *i, *oi;
	Rectangle r;
	Point p0, p;

	i = nil;
	menuing = TRUE;
	riosetcursor(&crosscursor);
	while(mouse->buttons == 0)
		readmouse(mousectl);
	p0 = onscreen(mouse->xy);
	p = p0;
	r.min = p;
	r.max = p;
	oi = nil;
	while(mouse->buttons == 4){
		if(!eqpt(mouse->xy, p)){
			p = onscreen(mouse->xy);
			r = canonrect(Rpt(p0, p));
			r = whichrect(r, p, whichcorner(r, p));
			if(Dx(r)>5 && Dy(r)>5){
				i = allocwindow(wscreen, r, Refnone, DNofill);
				freeimage(oi);
				if(i == nil)
					goto Rescue;
				oi = i;
				border(i, r, Selborder, sizecol, ZP);
				draw(i, insetrect(r, Selborder), cols[BACK], nil, ZP);
			}
		}
		readmouse(mousectl);
	}
	if(mouse->buttons != 0)
		goto Rescue;
	if(i==nil || !goodrect(r))
		goto Rescue;
	oi = i;
	i = allocwindow(wscreen, oi->r, Refbackup, DNofill);
	freeimage(oi);
	if(i == nil)
		goto Rescue;
	riosetcursor(corners[whichcorner(i->r, mouse->xy)]);
	goto Return;

 Rescue:
	riosetcursor(nil);
	freeimage(i);
	i = nil;
	flushimage(display, 1);
	while(mouse->buttons)
		readmouse(mousectl);

 Return:
	menuing = FALSE;
	return i;
}

void
drawedge(Image **bp, Rectangle r)
{
	Image *b = *bp;
	if(b != nil && Dx(b->r) == Dx(r) && Dy(b->r) == Dy(r))
		originwindow(b, r.min, r.min);
	else{
		freeimage(b);
		b = allocwindow(wscreen, r, Refbackup, DNofill);
		if(b != nil) draw(b, r, sizecol, nil, ZP);
		*bp = b;
	}
}

void
drawborder(Rectangle r, int show)
{
	static Image *b[4];
	int i;
	if(show == 0){
		for(i = 0; i < 4; i++){
			freeimage(b[i]);
			b[i] = nil;
		}
	}else{
		r = canonrect(r);
		drawedge(&b[0], Rect(r.min.x, r.min.y, r.min.x+Borderwidth, r.max.y));
		drawedge(&b[1], Rect(r.min.x+Borderwidth, r.min.y, r.max.x-Borderwidth, r.min.y+Borderwidth));
		drawedge(&b[2], Rect(r.max.x-Borderwidth, r.min.y, r.max.x, r.max.y));
		drawedge(&b[3], Rect(r.min.x+Borderwidth, r.max.y-Borderwidth, r.max.x-Borderwidth, r.max.y));
	}
}

Image*
drag(Window *w)
{
	Point p, op, d, dm, om;
	Rectangle r;

	menuing = TRUE;
	riosetcursor(&boxcursor);
	om = mouse->xy;
	dm = subpt(om, w->screenr.min);
	d = subpt(w->screenr.max, w->screenr.min);
	op = subpt(om, dm);
	drawborder(Rect(op.x, op.y, op.x+d.x, op.y+d.y), 1);
	while(mouse->buttons==4){
		p = subpt(mouse->xy, dm);
		if(!eqpt(p, op)){
			drawborder(Rect(p.x, p.y, p.x+d.x, p.y+d.y), 1);
			op = p;
		}
		readmouse(mousectl);
	}
	r = Rect(op.x, op.y, op.x+d.x, op.y+d.y);
	drawborder(r, 0);
	p = mouse->xy;
	riosetcursor(inborder(r, p) ? corners[whichcorner(r, p)] : nil);
	menuing = FALSE;
	if(mouse->buttons!=0 || !goodrect(r) || eqrect(r, w->screenr)){
		flushimage(display, 1);
		while(mouse->buttons)
			readmouse(mousectl);
		return nil;
	}
	return allocwindow(wscreen, r, Refbackup, DNofill);
}

Image*
bandsize(Window *w)
{
	Rectangle r, or;
	Point p, startp;
	int which, owhich, but;

	owhich = -1;
	or = w->screenr;
	but = mouse->buttons;
	startp = onscreen(mouse->xy);
	drawborder(or, 1);
	while(mouse->buttons == but) {
		p = onscreen(mouse->xy);
		which = whichcorner(or, p);
		if(which != owhich && which != 4 && (owhich|~which) & 1){
			owhich = which;
			riosetcursor(corners[which]);
		}
		r = whichrect(or, p, owhich);
		if(!eqrect(r, or) && goodrect(r)){
			or = r;
			drawborder(r, 1);
		}
		readmouse(mousectl);
	}
	drawborder(or, 0);
	if(mouse->buttons!=0 || !goodrect(or) || eqrect(or, w->screenr)
	|| abs(p.x-startp.x)+abs(p.y-startp.y) <= 1){
		flushimage(display, 1);
		while(mouse->buttons)
			readmouse(mousectl);
		return nil;
	}
	return allocwindow(wscreen, or, Refbackup, DNofill);
}

Window*
pointto(int wait)
{
	Window *w;

	menuing = TRUE;
	riosetcursor(&sightcursor);
	while(mouse->buttons == 0)
		readmouse(mousectl);
	if(mouse->buttons == 4)
		w = wpointto(mouse->xy);
	else
		w = nil;
	if(wait){
		while(mouse->buttons){
			if(mouse->buttons!=4 && w !=nil){	/* cancel */
				riosetcursor(nil);
				w = nil;
			}
			readmouse(mousectl);
		}
		if(w != nil && wpointto(mouse->xy) != w)
			w = nil;
	}
	riosetcursor(nil);
	menuing = FALSE;
	return w;
}

void
delete(void)
{
	Window *w;

	w = pointto(TRUE);
	if(w)
		wsendctlmesg(w, Deleted, ZR, nil);
}

void
resize(void)
{
	Window *w;
	Image *i;

	w = pointto(TRUE);
	if(w == nil)
		return;
	incref(w);
	i = sweep();
	if(i)
		wsendctlmesg(w, Reshaped, i->r, i);
	wclose(w);
}

void
move(void)
{
	Window *w;
	Image *i;

	w = pointto(FALSE);
	if(w == nil)
		return;
	incref(w);
	i = drag(w);
	if(i)
		wsendctlmesg(w, Reshaped, i->r, i);
	wclose(w);
}

int
whide(Window *w)
{
	Image *i;
	int j;

	for(j=0; j<nhidden; j++)
		if(hidden[j] == w)	/* already hidden */
			return -1;
	if(nhidden >= nelem(hidden))
		return 0;
	incref(w);
	i = allocimage(display, w->screenr, w->i->chan, 0, DNofill);
	if(i){
		hidden[nhidden++] = w;
		wsendctlmesg(w, Reshaped, ZR, i);
	}
	wclose(w);
	return i!=0;
}

int
wunhide(Window *w)
{
	int j;
	Image *i;

	for(j=0; j<nhidden; j++)
		if(hidden[j] == w)
			break;
	if(j == nhidden)
		return -1;	/* not hidden */
	incref(w);
	i = allocwindow(wscreen, w->i->r, Refbackup, DNofill);
	if(i){
		--nhidden;
		memmove(hidden+j, hidden+j+1, (nhidden-j)*sizeof(Window*));
		wsendctlmesg(w, Reshaped, w->i->r, i);
	}
	wclose(w);
	return i!=0;
}

void
hide(void)
{
	Window *w;

	w = pointto(TRUE);
	if(w)
		whide(w);
}

void
unhide(int j)
{
	Window *w;

	if(j < Hidden)
		return;
	j -= Hidden;
	w = hidden[j];
	if(w == nil)
		return;
	if(j < nhidden){
		wunhide(w);
		return;
	}
	/* uncover obscured window */
	for(j=0; j<nwindow; j++)
		if(window[j] == w){
			incref(w);
			wtopme(w);
			wcurrent(w);
			wclose(w);
			return;
		}
}

void
tile(void)
{
	Window *w;
	Image *im;
	Point o, n;
//  Point canvas;
	uint i, k, j; 
	uint ytiles = 0;
	uint xtiles = 1;

	// int s = 0;

	if (nwindow < 1) { 
		return; 
	} 

	for(k = 1; xtiles * ytiles < nwindow; k++) {
		switch(k % 2) {
			case 0:
				xtiles++;
				break;
			default:
				ytiles++;
				break;
		}
	}

	o = subpt(viewr.max, viewr.min);
	n = subpt(view->clipr.max, view->clipr.min);

/*
	unsigned int sw, sh, x, y;
	//canvas = subpt(view -> clipr.max, view -> clipr.min);
  canvas = subpt(viewr.max, viewr.min);

	x = canvas.x; 
	y = viewr.min.y;
//	x = view -> clipr.max.x;
//	y = view -> clipr.min.y;

	sw = canvas.x; 
	sh = canvas.y; 

  for(i = 0; i < nwindow; i++) {
		// print("X: %d, Y: %d, SW : %d, SH: %d, I : %d, NWindow: %d \n", x, y, sw, sh, i, nwindow);
		w = window[i];
//    qsort(w, nwindow, sizeof(window[0]), wtopcmp);

	  Rectangle r;

    if(i < nwindow - 1) {
      if(i % 2) {
        sh /= 2;
      } else {
        sw /= 2;
      } if((i % 4) == 2 && !s) {
        x += sw;
      } else if((i % 4) == 3 && !s) {
        y += sh; 
      }
    }
    if((i % 4) == 0) {
      if(s) { y += sh; } 
      else  { y -= sh; }
    } else if((i % 4) == 1) { 
      x += sw;
    } else if((i % 4) == 2) {
      y += sh;
    } else if((i % 4) == 3) {
      if(s) { x += sw; }
      else { x -= sw; }
    }

    if(i == 0) {
      if (nwindow != 1) { sw = canvas.x; } 
      y = canvas.y; 
    } else if(i == 1) {
      sw = canvas.x - sw;
    }
		r.min.x = x;
		r.min.y = y;

		r.max.x = sw;
		r.max.y = sh;

		for(j = 0; j < nhidden; ++j) {
			if(w == hidden[j])
				break;
		}

		incref(w);
		if(j > nhidden) {
			im = allocimage(display, r, screen->chan, 0, DNofill);
			r = ZR;
		} else { 
			im = allocwindow(wscreen, r, Refbackup, DNofill);
		} 

		if(im) {
			wsendctlmesg(w, Reshaped, r, im);
		}
		wclose(w);
	}
	flushimage(display, 1);
} */
	for(i = 0; i < nwindow; ++i) {
		w = window[i];
		Rectangle r;
		int sw, sh, x, y;

		sw = o.x / xtiles;
		sh = o.y / ytiles;
		x = sw * (i % xtiles);
		y = sh * ((i / xtiles) % ytiles);

		r.min.x = x;
		r.min.y = y;

		r.max.x = x + sw; 
		r.max.y = y + sh; 

		r = rectaddpt(r, screen->clipr.min);

		for(j = 0; j < nhidden; ++j)
			if(w == hidden[j])
				break;

		incref(w);
		if(j < nhidden) {
			im = allocimage(display, r, screen->chan, 0, DNofill);
		} else {
			 im = allocwindow(wscreen, r, Refbackup, DNofill);
		}

		if(im)
			wsendctlmesg(w, Reshaped, r, im);
		wclose(w);
	}

	flushimage(display, 1);
}

Window*
new(Image *i, int hideit, int scrollit, int pid, char *dir, char *cmd, char **argv)
{
	Window *w;
	Mousectl *mc;
	Channel *cm, *ck, *cctl, *cpid;
	void **arg;

	if(i == nil)
		return nil;
	if(hideit && nhidden >= nelem(hidden)){
		freeimage(i);
		return nil;
	}
	cm = chancreate(sizeof(Mouse), 0);
	ck = chancreate(sizeof(char*), 0);
	cctl = chancreate(sizeof(Wctlmesg), 4);
	cpid = chancreate(sizeof(int), 0);
	if(cm==nil || ck==nil || cctl==nil)
		error("new: channel alloc failed");
	mc = emalloc(sizeof(Mousectl));
	*mc = *mousectl;
	mc->image = i;
	mc->c = cm;
	w = wmk(i, mc, ck, cctl, scrollit);
	free(mc);	/* wmk copies *mc */
	window = erealloc(window, ++nwindow*sizeof(Window*));
	window[nwindow-1] = w;
	if(hideit){
		hidden[nhidden++] = w;
		w->screenr = ZR;
	}
	threadcreate(winctl, w, 8192);
	if(!hideit)
		wcurrent(w);
	if(pid == 0){
		arg = emalloc(5*sizeof(void*));
		arg[0] = w;
		arg[1] = cpid;
		arg[2] = cmd;
		if(argv == nil)
			arg[3] = rcargv;
		else
			arg[3] = argv;
		arg[4] = dir;
		proccreate(winshell, arg, 8192);
		pid = recvul(cpid);
		free(arg);
	}
	if(pid == 0){
		/* window creation failed */
		wsendctlmesg(w, Deleted, ZR, nil);
		chanfree(cpid);
		return nil;
	}
	wsetpid(w, pid, 1);
	wsetname(w);
	if(dir){
		free(w->dir);
		w->dir = estrdup(dir);
	}
	chanfree(cpid);
	return w;
}

static void
kbdproc(void *arg)
{
	Channel *c = arg;
	char buf[128], *p, *e;
	int fd, cfd, kfd, n;

	threadsetname("kbdproc");

	if((fd = open("/dev/cons", OREAD)) < 0){
		chanprint(c, "%r");
		return;
	}
	if((cfd = open("/dev/consctl", OWRITE)) < 0){
		chanprint(c, "%r");
		return;
	}
	fprint(cfd, "rawon");

	if(sendp(c, nil) <= 0)
		return;

	if((kfd = open("/dev/kbd", OREAD)) >= 0){
		close(fd);

		/* only serve a kbd file per window when we got one */
		servekbd = 1;

		/* read kbd state */
		while((n = read(kfd, buf, sizeof(buf)-1)) > 0){
			e = buf+n;
			e[-1] = 0;
			e[0] = 0;
			for(p = buf; p < e; p += strlen(p)+1)
				chanprint(c, "%s", p);
		}
	} else {
		/* read single characters */
		p = buf;
		for(;;){
			Rune r;

			e = buf + sizeof(buf);
			if((n = read(fd, p, e-p)) <= 0)
				break;
			e = p + n;
			while(p < e && fullrune(p, e - p)){
				p += chartorune(&r, p);
				if(r)
					chanprint(c, "c%C", r);
			}
			n = e - p;
			memmove(buf, p, n);
			p = buf + n;
		}
	}
	send(exitchan, nil);
}

Channel*
initkbd(void)
{
	Channel *c;
	char *e;

	c = chancreate(sizeof(char*), 16);
	procrfork(kbdproc, c, STACK, RFCFDG);
	if(e = recvp(c)){
		chanfree(c);
		c = nil;
		werrstr("%s", e);
		free(e);
	}
	return c;
}
