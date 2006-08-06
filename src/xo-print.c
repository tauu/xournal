#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <gtk/gtk.h>
#include <libgnomecanvas/libgnomecanvas.h>
#include <libgnomeprint/gnome-print-job.h>
#include <zlib.h>
#include <string.h>
#include <locale.h>

#include "xournal.h"
#include "xo-misc.h"
#include "xo-paint.h"
#include "xo-print.h"
#include "xo-file.h"

#define RGBA_RED(rgba) (((rgba>>24)&0xff)/255.0)
#define RGBA_GREEN(rgba) (((rgba>>16)&0xff)/255.0)
#define RGBA_BLUE(rgba) (((rgba>>8)&0xff)/255.0)
#define RGBA_ALPHA(rgba) (((rgba>>0)&0xff)/255.0)
#define RGBA_RGB(rgba) RGBA_RED(rgba), RGBA_GREEN(rgba), RGBA_BLUE(rgba)

/*********** Printing to PDF ************/

gboolean ispdfspace(char c)
{
  return (c==0 || c==9 || c==10 || c==12 || c==13 || c==' ');
}

gboolean ispdfdelim(char c)
{
  return (c=='(' || c==')' || c=='<' || c=='>' || c=='[' || c==']' ||
          c=='{' || c=='}' || c=='/' || c=='%');
}

void skipspace(char **p, char *eof)
{
  while (ispdfspace(**p) || **p=='%') {
    if (**p=='%') while (*p!=eof && **p!=10 && **p!=13) (*p)++;
    if (*p==eof) return;
    (*p)++;
  }
}

void free_pdfobj(struct PdfObj *obj)
{
  int i;
  
  if (obj==NULL) return;
  if ((obj->type == PDFTYPE_STRING || obj->type == PDFTYPE_NAME ||
      obj->type == PDFTYPE_STREAM) && obj->str!=NULL)
    g_free(obj->str);
  if ((obj->type == PDFTYPE_ARRAY || obj->type == PDFTYPE_DICT ||
      obj->type == PDFTYPE_STREAM) && obj->num>0) {
    for (i=0; i<obj->num; i++)
      free_pdfobj(obj->elts[i]);
    g_free(obj->elts);
  }
  if ((obj->type == PDFTYPE_DICT || obj->type == PDFTYPE_STREAM) && obj->num>0) {
    for (i=0; i<obj->num; i++)
      g_free(obj->names[i]);
    g_free(obj->names);
  }
  g_free(obj);
}

struct PdfObj *dup_pdfobj(struct PdfObj *obj)
{
  struct PdfObj *dup;
  int i;
  
  if (obj==NULL) return NULL;
  dup = g_memdup(obj, sizeof(struct PdfObj));
  if ((obj->type == PDFTYPE_STRING || obj->type == PDFTYPE_NAME ||
      obj->type == PDFTYPE_STREAM) && obj->str!=NULL) {
    if (obj->type == PDFTYPE_NAME) obj->len = strlen(obj->str);
    dup->str = g_memdup(obj->str, obj->len+1);
  }
  if ((obj->type == PDFTYPE_ARRAY || obj->type == PDFTYPE_DICT ||
      obj->type == PDFTYPE_STREAM) && obj->num>0) {
    dup->elts = g_malloc(obj->num*sizeof(struct PdfObj *));
    for (i=0; i<obj->num; i++)
      dup->elts[i] = dup_pdfobj(obj->elts[i]);
  }
  if ((obj->type == PDFTYPE_DICT || obj->type == PDFTYPE_STREAM) && obj->num>0) {
    dup->names = g_malloc(obj->num*sizeof(char *));
    for (i=0; i<obj->num; i++)
      dup->names[i] = g_strdup(obj->names[i]);
  }
  return dup;
}

void show_pdfobj(struct PdfObj *obj, GString *str)
{
  int i;
  if (obj==NULL) return;
  switch(obj->type) {
    case PDFTYPE_CST:
      if (obj->intval==1) g_string_append(str, "true");
      if (obj->intval==0) g_string_append(str, "false");
      if (obj->intval==-1) g_string_append(str, "null");
      break;
    case PDFTYPE_INT:
      g_string_append_printf(str, "%d", obj->intval);
      break;
    case PDFTYPE_REAL:
      g_string_append_printf(str, "%f", obj->realval);
      break;
    case PDFTYPE_STRING:
      g_string_append_len(str, obj->str, obj->len);
      break;
    case PDFTYPE_NAME:
      g_string_append(str, obj->str);
      break;
    case PDFTYPE_ARRAY:
      g_string_append_c(str, '[');
      for (i=0;i<obj->num;i++) {
        if (i) g_string_append_c(str, ' ');
        show_pdfobj(obj->elts[i], str);
      }
      g_string_append_c(str, ']');
      break;
    case PDFTYPE_DICT:
      g_string_append(str, "<<");
      for (i=0;i<obj->num;i++) {
        g_string_append_printf(str, " %s ", obj->names[i]); 
        show_pdfobj(obj->elts[i], str);
      }
      g_string_append(str, " >>");
      break;
    case PDFTYPE_REF:
      g_string_append_printf(str, "%d %d R", obj->intval, obj->num);
      break;
  }
}

void DEBUG_PRINTOBJ(struct PdfObj *obj)
{
  GString *s = g_string_new("");
  show_pdfobj(obj, s);
  puts(s->str);
  g_string_free(s, TRUE);
}

// parse a PDF object; returns NULL if fails
// THIS PARSER DOES NOT RECOGNIZE STREAMS YET

struct PdfObj *parse_pdf_object(char **ptr, char *eof)
{
  struct PdfObj *obj, *elt;
  char *p, *q, *r, *eltname;
  int stack;

  obj = g_malloc(sizeof(struct PdfObj));
  p = *ptr;
  skipspace(&p, eof);
  if (p==eof) { g_free(obj); return NULL; }
  
  // maybe a constant
  if (!strncmp(p, "true", 4)) {
    obj->type = PDFTYPE_CST;
    obj->intval = 1;
    *ptr = p+4;
    return obj;
  }
  if (!strncmp(p, "false", 5)) {
    obj->type = PDFTYPE_CST;
    obj->intval = 0;
    *ptr = p+5;
    return obj;
  }
  if (!strncmp(p, "null", 4)) {
    obj->type = PDFTYPE_CST;
    obj->intval = -1;
    *ptr = p+4;
    return obj;
  }

  // or a number ?
  obj->intval = strtol(p, &q, 10);
  *ptr = q;
  if (q!=p) {
    if (*q == '.') {
      obj->type = PDFTYPE_REAL;
      obj->realval = g_ascii_strtod(p, ptr);
      return obj;
    }
    if (ispdfspace(*q)) {
      // check for indirect reference
      skipspace(&q, eof);
      obj->num = strtol(q, &r, 10);
      if (r!=q) {
        skipspace(&r, eof);
        if (*r=='R') {
          *ptr = r+1;
          obj->type = PDFTYPE_REF;
          return obj;
        }
      }
    }
    obj->type = PDFTYPE_INT;
    return obj;
  }

  // a string ?
  if (*p=='(') {
    q=p+1; stack=1;
    while (stack>0 && q!=eof) {
      if (*q=='(') stack++;
      if (*q==')') stack--;
      if (*q=='\\') q++;
      if (q!=eof) q++;
    }
    if (q==eof) { g_free(obj); return NULL; }
    obj->type = PDFTYPE_STRING;
    obj->len = q-p;
    obj->str = g_malloc(obj->len+1);
    obj->str[obj->len] = 0;
    g_memmove(obj->str, p, obj->len);
    *ptr = q;
    return obj;
  }  
  if (*p=='<' && p[1]!='<') {
    q=p+1;
    while (*q!='>' && q!=eof) q++;
    if (q==eof) { g_free(obj); return NULL; }
    q++;
    obj->type = PDFTYPE_STRING;
    obj->len = q-p;
    obj->str = g_malloc(obj->len+1);
    obj->str[obj->len] = 0;
    g_memmove(obj->str, p, obj->len);
    *ptr = q;
    return obj;
  }
  
  // a name ?
  if (*p=='/') {
    q=p+1;
    while (!ispdfspace(*q) && !ispdfdelim(*q)) q++;
    obj->type = PDFTYPE_NAME;
    obj->str = g_strndup(p, q-p);
    *ptr = q;
    return obj;
  }

  // an array ?
  if (*p=='[') {
    obj->type = PDFTYPE_ARRAY;
    obj->num = 0;
    obj->elts = NULL;
    q=p+1; skipspace(&q, eof);
    while (*q!=']') {
      elt = parse_pdf_object(&q, eof);
      if (elt==NULL) { free_pdfobj(obj); return NULL; }
      obj->num++;
      obj->elts = g_realloc(obj->elts, obj->num*sizeof(struct PdfObj *));
      obj->elts[obj->num-1] = elt;
      skipspace(&q, eof);
    }
    *ptr = q+1;
    return obj;
  }

  // a dictionary ?
  if (*p=='<' && p[1]=='<') {
    obj->type = PDFTYPE_DICT;
    obj->num = 0;
    obj->elts = NULL;
    obj->names = NULL;
    q=p+2; skipspace(&q, eof);
    while (*q!='>' || q[1]!='>') {
      if (*q!='/') { free_pdfobj(obj); return NULL; }
      r=q+1;
      while (!ispdfspace(*r) && !ispdfdelim(*r)) r++;
      eltname = g_strndup(q, r-q);
      q=r; skipspace(&q, eof);
      elt = parse_pdf_object(&q, eof);
      if (elt==NULL) { g_free(eltname); free_pdfobj(obj); return NULL; }
      obj->num++;
      obj->elts = g_realloc(obj->elts, obj->num*sizeof(struct PdfObj *));
      obj->names = g_realloc(obj->names, obj->num*sizeof(char *));
      obj->elts[obj->num-1] = elt;
      obj->names[obj->num-1] = eltname;
      skipspace(&q, eof);
    }
    *ptr = q+2;
    return obj;
  }

  // DOES NOT RECOGNIZE STREAMS YET (handle as subcase of dictionary)
  
  g_free(obj);
  return NULL;
}

struct PdfObj *get_dict_entry(struct PdfObj *dict, char *name)
{
  int i;
  
  if (dict==NULL) return NULL;
  if (dict->type != PDFTYPE_DICT) return NULL;
  for (i=0; i<dict->num; i++) 
    if (!strcmp(dict->names[i], name)) return dict->elts[i];
  return NULL;
}

struct PdfObj *get_pdfobj(GString *pdfbuf, struct XrefTable *xref, struct PdfObj *obj)
{
  char *p, *eof;
  int offs, n;

  if (obj==NULL) return NULL;
  if (obj->type!=PDFTYPE_REF) return dup_pdfobj(obj);
  if (obj->intval>xref->last) return NULL;
  offs = xref->data[obj->intval];
  if (offs<=0 || offs >= pdfbuf->len) return NULL;

  p = pdfbuf->str + offs;
  eof = pdfbuf->str + pdfbuf->len;
  n = strtol(p, &p, 10);
  if (n!=obj->intval) return NULL;
  skipspace(&p, eof);
  n = strtol(p, &p, 10);
  skipspace(&p, eof);
  if (strncmp(p, "obj", 3)) return NULL;
  p+=3;
  return parse_pdf_object(&p, eof);
}

// read the xref table of a PDF file in memory, and return the trailerdict

struct PdfObj *parse_xref_table(GString *pdfbuf, struct XrefTable *xref, int offs)
{
  char *p, *q, *eof;
  struct PdfObj *trailerdict, *obj;
  int start, len, i;
  
  if (strncmp(pdfbuf->str+offs, "xref", 4)) return NULL;
  p = strstr(pdfbuf->str+offs, "trailer");
  eof = pdfbuf->str + pdfbuf->len;
  if (p==NULL) return NULL;
  p+=8;
  trailerdict = parse_pdf_object(&p, eof);
  obj = get_dict_entry(trailerdict, "/Size");
  if (obj!=NULL && obj->type == PDFTYPE_INT && obj->intval-1>xref->last)
    make_xref(xref, obj->intval-1, 0);
  obj = get_dict_entry(trailerdict, "/Prev");
  if (obj!=NULL && obj->type == PDFTYPE_INT && obj->intval>0 && obj->intval!=offs) {
    // recurse into older xref table
    obj = parse_xref_table(pdfbuf, xref, obj->intval);
    free_pdfobj(obj);
  }
  p = pdfbuf->str+offs+4;
  skipspace(&p, eof);
  if (*p<'0' || *p>'9') { free_pdfobj(trailerdict); return NULL; }
  while (*p>='0' && *p<='9') {
    start = strtol(p, &p, 10);
    skipspace(&p, eof);
    len = strtol(p, &p, 10);
    skipspace(&p, eof);
    if (len <= 0 || 20*len > eof-p) break;
    if (start+len-1 > xref->last) make_xref(xref, start+len-1, 0);
    for (i=start; i<start+len; i++) {
      xref->data[i] = strtol(p, NULL, 10);
      p+=20;
    }
    skipspace(&p, eof);
  }
  if (*p!='t') { free_pdfobj(trailerdict); return NULL; }
  return trailerdict;
}

// parse the page tree

int pdf_getpageinfo(GString *pdfbuf, struct XrefTable *xref, 
                struct PdfObj *pgtree, int nmax, struct PdfPageDesc *pages)
{
  struct PdfObj *obj, *kid;
  int i, count, j;
  
  obj = get_pdfobj(pdfbuf, xref, get_dict_entry(pgtree, "/Type"));
  if (obj == NULL || obj->type != PDFTYPE_NAME)
    return 0;
  if (!strcmp(obj->str, "/Page")) {
    free_pdfobj(obj);
    pages->contents = dup_pdfobj(get_dict_entry(pgtree, "/Contents"));
    obj = get_pdfobj(pdfbuf, xref, get_dict_entry(pgtree, "/Resources"));
    if (obj!=NULL) {
      free_pdfobj(pages->resources);
      pages->resources = obj;
    }
    obj = get_pdfobj(pdfbuf, xref, get_dict_entry(pgtree, "/MediaBox"));
    if (obj!=NULL) {
      free_pdfobj(pages->mediabox);
      pages->mediabox = obj;
    }
    obj = get_pdfobj(pdfbuf, xref, get_dict_entry(pgtree, "/Rotate"));
    if (obj!=NULL && obj->type == PDFTYPE_INT)
      pages->rotate = obj->intval;
    free_pdfobj(obj);
    return 1;
  }
  else if (!strcmp(obj->str, "/Pages")) {
    free_pdfobj(obj);
    obj = get_pdfobj(pdfbuf, xref, get_dict_entry(pgtree, "/Count"));
    if (obj!=NULL && obj->type == PDFTYPE_INT && 
        obj->intval>0 && obj->intval<=nmax) count = obj->intval;
    else count = 0;
    free_pdfobj(obj);
    obj = get_pdfobj(pdfbuf, xref, get_dict_entry(pgtree, "/Resources"));
    if (obj!=NULL)
      for (i=0; i<count; i++) {
        free_pdfobj(pages[i].resources);
        pages[i].resources = dup_pdfobj(obj);
      }
    free_pdfobj(obj);
    obj = get_pdfobj(pdfbuf, xref, get_dict_entry(pgtree, "/MediaBox"));
    if (obj!=NULL)
      for (i=0; i<count; i++) {
        free_pdfobj(pages[i].mediabox);
        pages[i].mediabox = dup_pdfobj(obj);
      }
    free_pdfobj(obj);
    obj = get_pdfobj(pdfbuf, xref, get_dict_entry(pgtree, "/Rotate"));
    if (obj!=NULL && obj->type == PDFTYPE_INT)
      for (i=0; i<count; i++)
        pages[i].rotate = obj->intval;
    free_pdfobj(obj);
    obj = get_pdfobj(pdfbuf, xref, get_dict_entry(pgtree, "/Kids"));
    if (obj!=NULL && obj->type == PDFTYPE_ARRAY) {
      for (i=0; i<obj->num; i++) {
        kid = get_pdfobj(pdfbuf, xref, obj->elts[i]);
        if (kid!=NULL) {
          j = pdf_getpageinfo(pdfbuf, xref, kid, nmax, pages);
          nmax -= j;
          pages += j;
          free_pdfobj(kid);
        }
      }
    }
    free_pdfobj(obj);
    return count;
  }
  return 0;
}

// parse a PDF file in memory

gboolean pdf_parse_info(GString *pdfbuf, struct PdfInfo *pdfinfo, struct XrefTable *xref)
{
  char *p;
  int i, offs;
  struct PdfObj *obj, *pages;

  xref->n_alloc = xref->last = 0;
  xref->data = NULL;
  p = pdfbuf->str + pdfbuf->len-1;
  
  while (*p!='s' && p!=pdfbuf->str) p--;
  if (strncmp(p, "startxref", 9)) return FALSE; // fail
  p+=9;
  while (ispdfspace(*p) && p!=pdfbuf->str+pdfbuf->len) p++;
  offs = strtol(p, NULL, 10);
  if (offs <= 0 || offs > pdfbuf->len) return FALSE; // fail
  pdfinfo->startxref = offs;
  
  pdfinfo->trailerdict = parse_xref_table(pdfbuf, xref, offs);
  if (pdfinfo->trailerdict == NULL) return FALSE; // fail
  
  obj = get_pdfobj(pdfbuf, xref,
     get_dict_entry(pdfinfo->trailerdict, "/Root"));
  if (obj == NULL)
    { free_pdfobj(pdfinfo->trailerdict); return FALSE; }
  pages = get_pdfobj(pdfbuf, xref, get_dict_entry(obj, "/Pages"));
  free_pdfobj(obj);
  if (pages == NULL)
    { free_pdfobj(pdfinfo->trailerdict); return FALSE; }
  obj = get_pdfobj(pdfbuf, xref, get_dict_entry(pages, "/Count"));
  if (obj == NULL || obj->type != PDFTYPE_INT || obj->intval<=0) 
    { free_pdfobj(pdfinfo->trailerdict); free_pdfobj(pages); 
      free_pdfobj(obj); return FALSE; }
  pdfinfo->npages = obj->intval;
  free_pdfobj(obj);
  
  pdfinfo->pages = g_malloc0(pdfinfo->npages*sizeof(struct PdfPageDesc));
  pdf_getpageinfo(pdfbuf, xref, pages, pdfinfo->npages, pdfinfo->pages);
  free_pdfobj(pages);
  
  return TRUE;
}

// add an entry to the xref table

void make_xref(struct XrefTable *xref, int nobj, int offset)
{
  if (xref->n_alloc <= nobj) {
    xref->n_alloc = nobj + 10;
    xref->data = g_realloc(xref->data, xref->n_alloc*sizeof(int));
  }
  if (xref->last < nobj) xref->last = nobj;
  xref->data[nobj] = offset;
}

// a wrapper for deflate

GString *do_deflate(char *in, int len)
{
  GString *out;
  z_stream zs;
  
  zs.zalloc = Z_NULL;
  zs.zfree = Z_NULL;
  deflateInit(&zs, Z_DEFAULT_COMPRESSION);
  zs.next_in = (Bytef *)in;
  zs.avail_in = len;
  zs.avail_out = deflateBound(&zs, len);
  out = g_string_sized_new(zs.avail_out);
  zs.next_out = (Bytef *)out->str;
  deflate(&zs, Z_FINISH);
  out->len = zs.total_out;
  deflateEnd(&zs);
  return out;
}

// prefix to scale the original page

GString *make_pdfprefix(struct PdfPageDesc *pgdesc, double width, double height)
{
  GString *str;
  double v[4], t, xscl, yscl;
  int i;
  
  str = g_string_new("q ");
  if (pgdesc->rotate == 90) {
    g_string_append_printf(str, "0 -1 1 0 0 %.2f cm ", height);
    t = height; height = width; width = t;
  }
  if (pgdesc->rotate == 270) {
    g_string_append_printf(str, "0 1 -1 0 %.2f 0 cm ", width);
    t = height; height = width; width = t;
  }
  if (pgdesc->rotate == 180) {
    g_string_append_printf(str, "-1 0 0 -1 %.2f %.2f cm ", width, height);
  }
  if (pgdesc->mediabox==NULL || pgdesc->mediabox->type != PDFTYPE_ARRAY ||
      pgdesc->mediabox->num != 4) return str;
  for (i=0; i<4; i++) {
    if (pgdesc->mediabox->elts[i]->type == PDFTYPE_INT)
      v[i] = pgdesc->mediabox->elts[i]->intval;
    else if (pgdesc->mediabox->elts[i]->type == PDFTYPE_REAL)
      v[i] = pgdesc->mediabox->elts[i]->realval;
    else return str;
  }
  if (v[0]>v[2]) { t = v[0]; v[0] = v[2]; v[2] = t; }
  if (v[1]>v[3]) { t = v[1]; v[1] = v[3]; v[3] = t; }
  if (v[2]-v[0] < 1. || v[3]-v[1] < 1.) return str;
  xscl = width/(v[2]-v[0]);
  yscl = height/(v[3]-v[1]);
  g_string_append_printf(str, "%.4f 0 0 %.4f %.2f %.2f cm ",
    xscl, yscl, -v[0]*xscl, -v[1]*yscl);
  return str;
}

// add an entry to a subentry of a directory

struct PdfObj *mk_pdfname(char *name)
{
  struct PdfObj *obj;
  
  obj = g_malloc(sizeof(struct PdfObj));
  obj->type = PDFTYPE_NAME;
  obj->str = g_strdup(name);
  return obj;
}

struct PdfObj *mk_pdfref(int num)
{
  struct PdfObj *obj;
  
  obj = g_malloc(sizeof(struct PdfObj));
  obj->type = PDFTYPE_REF;
  obj->intval = num;
  obj->num = 0;
  return obj;
}

gboolean iseq_obj(struct PdfObj *a, struct PdfObj *b)
{
  if (a==NULL || b==NULL) return (a==b);
  if (a->type!=b->type) return FALSE;
  if (a->type == PDFTYPE_CST || a->type == PDFTYPE_INT)
    return (a->intval == b->intval);
  if (a->type == PDFTYPE_REAL)
    return (a->realval == b->realval);
  if (a->type == PDFTYPE_NAME)
    return !strcmp(a->str, b->str);
  if (a->type == PDFTYPE_REF)
    return (a->intval == b->intval && a->num == b->num);
  return FALSE;
}

void add_dict_subentry(GString *pdfbuf, struct XrefTable *xref,
   struct PdfObj *obj, char *section, int type, char *name, struct PdfObj *entry)
{
  struct PdfObj *sec;
  int i, subpos;
  
  subpos = -1;
  for (i=0; i<obj->num; i++) 
    if (!strcmp(obj->names[i], section)) subpos = i;
  if (subpos == -1) {
    subpos = obj->num;
    obj->num++;
    obj->elts = g_realloc(obj->elts, obj->num*sizeof(struct PdfObj*));
    obj->names = g_realloc(obj->names, obj->num*sizeof(char *));
    obj->names[subpos] = g_strdup(section);
    obj->elts[subpos] = NULL;
  }
  if (obj->elts[subpos]!=NULL && obj->elts[subpos]->type==PDFTYPE_REF) {
    sec = get_pdfobj(pdfbuf, xref, obj->elts[subpos]);
    free_pdfobj(obj->elts[subpos]);
    obj->elts[subpos] = sec;
  }
  if (obj->elts[subpos]!=NULL && obj->elts[subpos]->type!=type)
    { free_pdfobj(obj->elts[subpos]); obj->elts[subpos] = NULL; }
  if (obj->elts[subpos] == NULL) {
    obj->elts[subpos] = sec = g_malloc(sizeof(struct PdfObj));
    sec->type = type;
    sec->num = 0;
    sec->elts = NULL;
    sec->names = NULL;
  }
  sec = obj->elts[subpos];

  subpos = -1;
  if (type==PDFTYPE_DICT) {
    for (i=0; i<sec->num; i++) 
      if (!strcmp(sec->names[i], name)) subpos = i;
    if (subpos == -1) {
      subpos = sec->num;
      sec->num++;
      sec->elts = g_realloc(sec->elts, sec->num*sizeof(struct PdfObj*));
      sec->names = g_realloc(sec->names, sec->num*sizeof(char *));
      sec->names[subpos] = g_strdup(name);
      sec->elts[subpos] = NULL;
    }
    free_pdfobj(sec->elts[subpos]);
    sec->elts[subpos] = entry;
  } 
  if (type==PDFTYPE_ARRAY) {
    for (i=0; i<sec->num; i++)
      if (iseq_obj(sec->elts[i], entry)) subpos = i;
    if (subpos == -1) {
      subpos = sec->num;
      sec->num++;
      sec->elts = g_realloc(sec->elts, sec->num*sizeof(struct PdfObj*));
      sec->elts[subpos] = entry;
    }
    else free_pdfobj(entry);
  }
}

// draw a page's background

void pdf_draw_solid_background(struct Page *pg, GString *str)
{
  double x, y;

  g_string_append_printf(str, 
    "%.2f %.2f %.2f rg 0 0 %.2f %.2f re f ",
    RGBA_RGB(pg->bg->color_rgba), pg->width, pg->height);
  if (pg->bg->ruling == RULING_NONE) return;
  g_string_append_printf(str,
    "%.2f %.2f %.2f RG %.2f w ",
    RGBA_RGB(RULING_COLOR), RULING_THICKNESS);
  if (pg->bg->ruling == RULING_GRAPH) {
    for (x=RULING_GRAPHSPACING; x<pg->width-1; x+=RULING_GRAPHSPACING)
      g_string_append_printf(str, "%.2f 0 m %.2f %.2f l S ",
        x, x, pg->height);
    for (y=RULING_GRAPHSPACING; y<pg->height-1; y+=RULING_GRAPHSPACING)
      g_string_append_printf(str, "0 %.2f m %.2f %.2f l S ",
        y, pg->width, y);
    return;
  }
  for (y=RULING_TOPMARGIN; y<pg->height-1; y+=RULING_SPACING)
    g_string_append_printf(str, "0 %.2f m %.2f %.2f l S ",
      y, pg->width, y);
  if (pg->bg->ruling == RULING_LINED)
    g_string_append_printf(str, 
      "%.2f %.2f %.2f RG %.2f 0 m %.2f %.2f l S ",
      RGBA_RGB(RULING_MARGIN_COLOR), 
      RULING_LEFTMARGIN, RULING_LEFTMARGIN, pg->height);
}

int pdf_draw_bitmap_background(struct Page *pg, GString *str, 
                                struct XrefTable *xref, GString *pdfbuf)
{
  BgPdfPage *pgpdf;
  GdkPixbuf *pix;
  GString *zpix;
  char *buf, *p1, *p2;
  int height, width, stride, x, y, chan;
  
  if (pg->bg->type == BG_PDF) {
    pgpdf = (struct BgPdfPage *)g_list_nth_data(bgpdf.pages, pg->bg->file_page_seq-1);
    if (pgpdf == NULL) return -1;
    if (pgpdf->dpi != PDFTOPPM_PRINTING_DPI) {
      add_bgpdf_request(pg->bg->file_page_seq, 0, TRUE);
      while (pgpdf->dpi != PDFTOPPM_PRINTING_DPI && bgpdf.status == STATUS_RUNNING)
        gtk_main_iteration();
    }
    pix = pgpdf->pixbuf;
  }
  else pix = pg->bg->pixbuf;
  
  if (gdk_pixbuf_get_bits_per_sample(pix) != 8) return -1;
  if (gdk_pixbuf_get_colorspace(pix) != GDK_COLORSPACE_RGB) return -1;
  
  width = gdk_pixbuf_get_width(pix);
  height = gdk_pixbuf_get_height(pix);
  stride = gdk_pixbuf_get_rowstride(pix);
  chan = gdk_pixbuf_get_n_channels(pix);
  if (chan!=3 && chan!=4) return -1;

  g_string_append_printf(str, "q %.2f 0 0 %.2f 0 %.2f cm /ImBg Do Q ",
    pg->width, -pg->height, pg->height);
  
  p2 = buf = (char *)g_malloc(3*width*height);
  for (y=0; y<height; y++) {
    p1 = (char *)gdk_pixbuf_get_pixels(pix)+stride*y;
    for (x=0; x<width; x++) {
      *(p2++)=*(p1++); *(p2++)=*(p1++); *(p2++)=*(p1++);
      if (chan==4) p1++;
    }
  }
  zpix = do_deflate(buf, 3*width*height);
  g_free(buf);

  make_xref(xref, xref->last+1, pdfbuf->len);
  g_string_append_printf(pdfbuf, 
    "%d 0 obj\n<< /Length %d /Filter /FlateDecode /Type /Xobject "
    "/Subtype /Image /Width %d /Height %d /ColorSpace /DeviceRGB "
    "/BitsPerComponent 8 >> stream\n",
    xref->last, zpix->len, width, height);
  g_string_append_len(pdfbuf, zpix->str, zpix->len);
  g_string_free(zpix, TRUE);
  g_string_append(pdfbuf, "endstream\nendobj\n");
 
  return xref->last;
}

// draw a page's graphics

void pdf_draw_page(struct Page *pg, GString *str, gboolean *use_hiliter)
{
  GList *layerlist, *itemlist;
  struct Layer *l;
  struct Item *item;
  guint old_rgba;
  double old_thickness;
  double *pt;
  int i;
  
  old_rgba = 0x12345678;    // not any values we use, so we'll reset them
  old_thickness = 0.0;

  for (layerlist = pg->layers; layerlist!=NULL; layerlist = layerlist->next) {
    l = (struct Layer *)layerlist->data;
    for (itemlist = l->items; itemlist!=NULL; itemlist = itemlist->next) {
      item = (struct Item *)itemlist->data;
      if (item->type == ITEM_STROKE) {
        if ((item->brush.color_rgba & ~0xff) != old_rgba)
          g_string_append_printf(str, "%.2f %.2f %.2f RG ",
            RGBA_RGB(item->brush.color_rgba));
        if (item->brush.thickness != old_thickness)
          g_string_append_printf(str, "%.2f w ", item->brush.thickness);
        if ((item->brush.color_rgba & 0xf0) != 0xf0) { // transparent
          g_string_append(str, "q /XoHi gs ");
          *use_hiliter = TRUE;
        }
        old_rgba = item->brush.color_rgba & ~0xff;
        old_thickness = item->brush.thickness;
        pt = item->path->coords;
        g_string_append_printf(str, "%.2f %.2f m ", pt[0], pt[1]);
        for (i=1, pt+=2; i<item->path->num_points; i++, pt+=2)
          g_string_append_printf(str, "%.2f %.2f l ", pt[0], pt[1]);
        g_string_append_printf(str,"S\n");
        if ((item->brush.color_rgba & 0xf0) != 0xf0) // undo transparent
          g_string_append(str, "Q ");
      }
    }
  }
}

// main printing function

/* we use the following object numbers, starting with n_obj_catalog:
    0 the document catalog
    1 the page tree
    2 the GS for the hiliters
    3 ... the page objects
*/

gboolean print_to_pdf(char *filename)
{
  FILE *f;
  GString *pdfbuf, *pgstrm, *zpgstrm, *tmpstr;
  int n_obj_catalog, n_obj_pages_offs, n_page, n_obj_bgpix, n_obj_prefix;
  int i, startxref;
  struct XrefTable xref;
  GList *pglist;
  struct Page *pg;
  char *buf;
  unsigned int len;
  gboolean annot, uses_pdf;
  gboolean use_hiliter;
  struct PdfInfo pdfinfo;
  struct PdfObj *obj;
  
  f = fopen(filename, "w");
  if (f == NULL) return FALSE;
  setlocale(LC_NUMERIC, "C");
  annot = FALSE;
  xref.data = NULL;
  uses_pdf = FALSE;
  for (pglist = journal.pages; pglist!=NULL; pglist = pglist->next) {
    pg = (struct Page *)pglist->data;
    if (pg->bg->type == BG_PDF) uses_pdf = TRUE;
  }
  
  if (uses_pdf && bgpdf.status != STATUS_NOT_INIT && 
      g_file_get_contents(bgpdf.tmpfile_copy, &buf, &len, NULL) &&
      !strncmp(buf, "%PDF-1.", 7)) {
    // parse the existing PDF file
    pdfbuf = g_string_new_len(buf, len);
    g_free(buf);
    if (pdfbuf->str[7]<'4') pdfbuf->str[7] = '4'; // upgrade to 1.4
    annot = pdf_parse_info(pdfbuf, &pdfinfo, &xref);
    if (!annot) {
      g_string_free(pdfbuf, TRUE);
      if (xref.data != NULL) g_free(xref.data);
    }
  }

  if (!annot) {
    pdfbuf = g_string_new("%PDF-1.4\n%\370\357\365\362\n");
    xref.n_alloc = xref.last = 0;
    xref.data = NULL;
  }
    
  // catalog and page tree
  n_obj_catalog = xref.last+1;
  n_obj_pages_offs = xref.last+4;
  make_xref(&xref, n_obj_catalog, pdfbuf->len);
  g_string_append_printf(pdfbuf, 
    "%d 0 obj\n<< /Type /Catalog /Pages %d 0 R >> endobj\n",
     n_obj_catalog, n_obj_catalog+1);
  make_xref(&xref, n_obj_catalog+1, pdfbuf->len);
  g_string_append_printf(pdfbuf,
    "%d 0 obj\n<< /Type /Pages /Kids [", n_obj_catalog+1);
  for (i=0;i<journal.npages;i++)
    g_string_append_printf(pdfbuf, "%d 0 R ", n_obj_pages_offs+i);
  g_string_append_printf(pdfbuf, "] /Count %d >> endobj\n", journal.npages);
  make_xref(&xref, n_obj_catalog+2, pdfbuf->len);
  g_string_append_printf(pdfbuf, 
    "%d 0 obj\n<< /Type /ExtGState /CA 0.5 >> endobj\n",
     n_obj_catalog+2);
  xref.last = n_obj_pages_offs + journal.npages-1;
  
  for (pglist = journal.pages, n_page = 0; pglist!=NULL;
       pglist = pglist->next, n_page++) {
    pg = (struct Page *)pglist->data;
    
    // draw the background and page into pgstrm
    pgstrm = g_string_new("");
    g_string_printf(pgstrm, "q 1 0 0 -1 0 %.2f cm 1 J 1 j ", pg->height);
    n_obj_bgpix = -1;
    n_obj_prefix = -1;
    if (pg->bg->type == BG_SOLID)
      pdf_draw_solid_background(pg, pgstrm);
    else if (pg->bg->type == BG_PDF && annot && 
             pdfinfo.pages[pg->bg->file_page_seq-1].contents!=NULL) {
      make_xref(&xref, xref.last+1, pdfbuf->len);
      n_obj_prefix = xref.last;
      tmpstr = make_pdfprefix(pdfinfo.pages+(pg->bg->file_page_seq-1),
                              pg->width, pg->height);
      g_string_append_printf(pdfbuf,
        "%d 0 obj\n<< /Length %d >> stream\n%s\nendstream\nendobj\n",
        n_obj_prefix, tmpstr->len, tmpstr->str);
      g_string_free(tmpstr, TRUE);
      g_string_prepend(pgstrm, "Q ");
    }
    else if (pg->bg->type == BG_PIXMAP || pg->bg->type == BG_PDF)
      n_obj_bgpix = pdf_draw_bitmap_background(pg, pgstrm, &xref, pdfbuf);
    // draw the page contents
    use_hiliter = FALSE;
    pdf_draw_page(pg, pgstrm, &use_hiliter);
    g_string_append_printf(pgstrm, "Q\n");
    
    // deflate pgstrm and write it
    zpgstrm = do_deflate(pgstrm->str, pgstrm->len);
    g_string_free(pgstrm, TRUE);
    
    make_xref(&xref, xref.last+1, pdfbuf->len);
    g_string_append_printf(pdfbuf, 
      "%d 0 obj\n<< /Length %d /Filter /FlateDecode>> stream\n",
      xref.last, zpgstrm->len);
    g_string_append_len(pdfbuf, zpgstrm->str, zpgstrm->len);
    g_string_free(zpgstrm, TRUE);
    g_string_append(pdfbuf, "endstream\nendobj\n");
    
    // write the page object
    
    make_xref(&xref, n_obj_pages_offs+n_page, pdfbuf->len);
    g_string_append_printf(pdfbuf, 
      "%d 0 obj\n<< /Type /Page /Parent %d 0 R /MediaBox [0 0 %.2f %.2f] ",
      n_obj_pages_offs+n_page, n_obj_catalog+1, pg->width, pg->height);
    if (n_obj_prefix>0) {
      obj = get_pdfobj(pdfbuf, &xref, pdfinfo.pages[pg->bg->file_page_seq-1].contents);
      if (obj->type != PDFTYPE_ARRAY) {
        free_pdfobj(obj);
        obj = dup_pdfobj(pdfinfo.pages[pg->bg->file_page_seq-1].contents);
      }
      g_string_append_printf(pdfbuf, "/Contents [%d 0 R ", n_obj_prefix);
      if (obj->type == PDFTYPE_REF) 
        g_string_append_printf(pdfbuf, "%d %d R ", obj->intval, obj->num);
      if (obj->type == PDFTYPE_ARRAY) {
        for (i=0; i<obj->num; i++) {
          show_pdfobj(obj->elts[i], pdfbuf);
          g_string_append_c(pdfbuf, ' ');
        }
      }
      free_pdfobj(obj);
      g_string_append_printf(pdfbuf, "%d 0 R] ", xref.last);
    }
    else g_string_append_printf(pdfbuf, "/Contents %d 0 R ", xref.last);
    g_string_append(pdfbuf, "/Resources ");

    if (n_obj_prefix>0)
      obj = dup_pdfobj(pdfinfo.pages[pg->bg->file_page_seq-1].resources);
    else obj = NULL;
    if (obj!=NULL && obj->type!=PDFTYPE_DICT)
      { free_pdfobj(obj); obj=NULL; }
    if (obj==NULL) {
      obj = g_malloc(sizeof(struct PdfObj));
      obj->type = PDFTYPE_DICT;
      obj->num = 0;
      obj->elts = NULL;
      obj->names = NULL;
    }
    add_dict_subentry(pdfbuf, &xref,
        obj, "/ProcSet", PDFTYPE_ARRAY, NULL, mk_pdfname("/PDF"));
    if (n_obj_bgpix>0)
      add_dict_subentry(pdfbuf, &xref,
        obj, "/ProcSet", PDFTYPE_ARRAY, NULL, mk_pdfname("/ImageC"));
    if (use_hiliter)
      add_dict_subentry(pdfbuf, &xref,
        obj, "/ExtGState", PDFTYPE_DICT, "/XoHi", mk_pdfref(n_obj_catalog+2));
    if (n_obj_bgpix>0)
      add_dict_subentry(pdfbuf, &xref,
        obj, "/XObject", PDFTYPE_DICT, "/ImBg", mk_pdfref(n_obj_bgpix));
    show_pdfobj(obj, pdfbuf);
    free_pdfobj(obj);
    g_string_append(pdfbuf, " >> endobj\n");
  }
  
  // PDF trailer
  startxref = pdfbuf->len;
  if (annot) g_string_append_printf(pdfbuf,
        "xref\n%d %d\n", n_obj_catalog, xref.last-n_obj_catalog+1);
  else g_string_append_printf(pdfbuf, 
        "xref\n0 %d\n0000000000 65535 f \n", xref.last+1);
  for (i=n_obj_catalog; i<=xref.last; i++)
    g_string_append_printf(pdfbuf, "%010d 00000 n \n", xref.data[i]);
  g_string_append_printf(pdfbuf, 
    "trailer\n<< /Size %d /Root %d 0 R ", xref.last+1, n_obj_catalog);
  if (annot) {
    g_string_append_printf(pdfbuf, "/Prev %d ", pdfinfo.startxref);
    // keeping encryption info somehow doesn't work.
    // xournal can't annotate encrypted PDFs anyway...
/*    
    obj = get_dict_entry(pdfinfo.trailerdict, "/Encrypt");
    if (obj!=NULL) {
      g_string_append_printf(pdfbuf, "/Encrypt ");
      show_pdfobj(obj, pdfbuf);
    } 
*/
  }
  g_string_append_printf(pdfbuf, 
    ">>\nstartxref\n%d\n%%%%EOF\n", startxref);
  
  g_free(xref.data);
  if (annot) {
    free_pdfobj(pdfinfo.trailerdict);
    if (pdfinfo.pages!=NULL)
      for (i=0; i<pdfinfo.npages; i++) {
        free_pdfobj(pdfinfo.pages[i].resources);
        free_pdfobj(pdfinfo.pages[i].mediabox);
        free_pdfobj(pdfinfo.pages[i].contents);
      }
  }
  
  setlocale(LC_NUMERIC, "");
  if (fwrite(pdfbuf->str, 1, pdfbuf->len, f) < pdfbuf->len) {
    fclose(f);
    g_string_free(pdfbuf, TRUE);
    return FALSE;
  }
  fclose(f);
  g_string_free(pdfbuf, TRUE);
  return TRUE;
}

/*********** Printing via libgnomeprint **********/

// does the same job as update_canvas_bg(), but to a print context

void print_background(GnomePrintContext *gpc, struct Page *pg, gboolean *abort)
{
  double x, y;
  GdkPixbuf *pix;
  BgPdfPage *pgpdf;

  if (pg->bg->type == BG_SOLID) {
    gnome_print_setopacity(gpc, 1.0);
    gnome_print_setrgbcolor(gpc, RGBA_RGB(pg->bg->color_rgba));
    gnome_print_rect_filled(gpc, 0, 0, pg->width, pg->height);

    if (pg->bg->ruling == RULING_NONE) return;
    gnome_print_setrgbcolor(gpc, RGBA_RGB(RULING_COLOR));
    gnome_print_setlinewidth(gpc, RULING_THICKNESS);
    
    if (pg->bg->ruling == RULING_GRAPH) {
      for (x=RULING_GRAPHSPACING; x<pg->width-1; x+=RULING_GRAPHSPACING)
        gnome_print_line_stroked(gpc, x, 0, x, pg->height);
      for (y=RULING_GRAPHSPACING; y<pg->height-1; y+=RULING_GRAPHSPACING)
        gnome_print_line_stroked(gpc, 0, y, pg->width, y);
      return;
    }
    
    for (y=RULING_TOPMARGIN; y<pg->height-1; y+=RULING_SPACING)
      gnome_print_line_stroked(gpc, 0, y, pg->width, y);
    if (pg->bg->ruling == RULING_LINED) {
      gnome_print_setrgbcolor(gpc, RGBA_RGB(RULING_MARGIN_COLOR));
      gnome_print_line_stroked(gpc, RULING_LEFTMARGIN, 0, RULING_LEFTMARGIN, pg->height);
    }
    return;
  }
  else if (pg->bg->type == BG_PIXMAP || pg->bg->type == BG_PDF) {
    if (pg->bg->type == BG_PDF) {
      pgpdf = (struct BgPdfPage *)g_list_nth_data(bgpdf.pages, pg->bg->file_page_seq-1);
      if (pgpdf == NULL) return;
      if (pgpdf->dpi != PDFTOPPM_PRINTING_DPI) {
        add_bgpdf_request(pg->bg->file_page_seq, 0, TRUE);
        while (pgpdf->dpi != PDFTOPPM_PRINTING_DPI && bgpdf.status == STATUS_RUNNING) {
          gtk_main_iteration();
          if (*abort) return;
        }
      }
      pix = pgpdf->pixbuf;
    }
    else pix = pg->bg->pixbuf;
    if (gdk_pixbuf_get_bits_per_sample(pix) != 8) return;
    if (gdk_pixbuf_get_colorspace(pix) != GDK_COLORSPACE_RGB) return;
    gnome_print_gsave(gpc);
    gnome_print_scale(gpc, pg->width, -pg->height);
    gnome_print_translate(gpc, 0., -1.);
    if (gdk_pixbuf_get_n_channels(pix) == 3)
       gnome_print_rgbimage(gpc, gdk_pixbuf_get_pixels(pix),
         gdk_pixbuf_get_width(pix), gdk_pixbuf_get_height(pix), gdk_pixbuf_get_rowstride(pix));
    else if (gdk_pixbuf_get_n_channels(pix) == 4)
       gnome_print_rgbaimage(gpc, gdk_pixbuf_get_pixels(pix),
         gdk_pixbuf_get_width(pix), gdk_pixbuf_get_height(pix), gdk_pixbuf_get_rowstride(pix));
    gnome_print_grestore(gpc);
    return;
  }
}

void print_page(GnomePrintContext *gpc, struct Page *pg, int pageno,
                double pgwidth, double pgheight, gboolean *abort)
{
  char tmp[10];
  gdouble scale;
  guint old_rgba;
  double old_thickness;
  GList *layerlist, *itemlist;
  struct Layer *l;
  struct Item *item;
  int i;
  double *pt;
  
  if (pg==NULL) return;
  
  g_snprintf(tmp, 10, "Page %d", pageno);
  gnome_print_beginpage(gpc, (guchar *)tmp);
  gnome_print_gsave(gpc);
  
  scale = MIN(pgwidth/pg->width, pgheight/pg->height)*0.95;
  gnome_print_translate(gpc,
     (pgwidth - scale*pg->width)/2, (pgheight + scale*pg->height)/2);
  gnome_print_scale(gpc, scale, -scale);
  gnome_print_setlinejoin(gpc, 1); // round
  gnome_print_setlinecap(gpc, 1); // round

  print_background(gpc, pg, abort);

  old_rgba = 0x12345678;    // not any values we use, so we'll reset them
  old_thickness = 0.0;

  for (layerlist = pg->layers; layerlist!=NULL; layerlist = layerlist->next) {
    if (*abort) break;
    l = (struct Layer *)layerlist->data;
    for (itemlist = l->items; itemlist!=NULL; itemlist = itemlist->next) {
      if (*abort) break;
      item = (struct Item *)itemlist->data;
      if (item->type == ITEM_STROKE) {
        if ((item->brush.color_rgba & ~0xff) != (old_rgba & ~0xff))
          gnome_print_setrgbcolor(gpc, RGBA_RGB(item->brush.color_rgba));
        if ((item->brush.color_rgba & 0xff) != (old_rgba & 0xff))
          gnome_print_setopacity(gpc, RGBA_ALPHA(item->brush.color_rgba));
        if (item->brush.thickness != old_thickness)
          gnome_print_setlinewidth(gpc, item->brush.thickness);
        old_rgba = item->brush.color_rgba;
        old_thickness = item->brush.thickness;
        gnome_print_newpath(gpc);
        pt = item->path->coords;
        gnome_print_moveto(gpc, pt[0], pt[1]);
        for (i=1, pt+=2; i<item->path->num_points; i++, pt+=2)
          gnome_print_lineto(gpc, pt[0], pt[1]);
        gnome_print_stroke(gpc);
      }
    }
  }
  
  gnome_print_grestore(gpc);
  gnome_print_showpage(gpc);
}

void cb_print_abort(GtkDialog *dialog, gint response, gboolean *abort)
{
  *abort = TRUE;
}

void print_job_render(GnomePrintJob *gpj, int fromPage, int toPage)
{
  GnomePrintConfig *config;
  GnomePrintContext *gpc;
  GtkWidget *wait_dialog;
  double pgwidth, pgheight;
  int i;
  gboolean abort;
  
  config = gnome_print_job_get_config(gpj);
  gnome_print_config_get_page_size(config, &pgwidth, &pgheight);
  g_object_unref(G_OBJECT(config));

  gpc = gnome_print_job_get_context(gpj);

  abort = FALSE;
  wait_dialog = gtk_message_dialog_new(GTK_WINDOW(winMain), GTK_DIALOG_MODAL,
     GTK_MESSAGE_INFO, GTK_BUTTONS_CANCEL, "Preparing print job");
  gtk_widget_show(wait_dialog);
  g_signal_connect(wait_dialog, "response", G_CALLBACK (cb_print_abort), &abort);
  
  for (i = fromPage; i <= toPage; i++) {
#if GTK_CHECK_VERSION(2,6,0)
    if (!gtk_check_version(2, 6, 0))
      gtk_message_dialog_format_secondary_text(
             GTK_MESSAGE_DIALOG(wait_dialog), "Page %d", i+1); 
#endif
    while (gtk_events_pending()) gtk_main_iteration();
    print_page(gpc, (struct Page *)g_list_nth_data(journal.pages, i), i+1,
                                             pgwidth, pgheight, &abort);
    if (abort) break;
  }
#if GTK_CHECK_VERSION(2,6,0)
  if (!gtk_check_version(2, 6, 0))
    gtk_message_dialog_format_secondary_text(
              GTK_MESSAGE_DIALOG(wait_dialog), "Finalizing...");
#endif
  while (gtk_events_pending()) gtk_main_iteration();

  gnome_print_context_close(gpc);  
  g_object_unref(G_OBJECT(gpc));  

  gnome_print_job_close(gpj);
  if (!abort) gnome_print_job_print(gpj);
  g_object_unref(G_OBJECT(gpj));

  gtk_widget_destroy(wait_dialog);
}
