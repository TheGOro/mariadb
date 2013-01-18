/*************** Catalog H Declares Source Code File (.H) **************/
/*  Name: CATALOG.H  Version 3.2                                       */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          2000-2012    */
/*                                                                     */
/*  This file contains the CATALOG PlugDB classes definitions.         */
/***********************************************************************/
#ifndef __CATALOG__H
#define	__CATALOG__H

#include "block.h"

/***********************************************************************/
/*  Defines the length of a buffer to contain entire table section.    */
/***********************************************************************/
#define PLG_MAX_PATH    144   /* Must be the same across systems       */
#define PLG_BUFF_LEN    100   /* Number of lines in binary file buffer */

#if !defined(WIN32)
/**************************************************************************/
/*  Defines specific to Windows and ODBC.                                 */
/**************************************************************************/
#define SQL_CHAR              1
#define SQL_NUMERIC           2
#define SQL_DECIMAL           3
#define SQL_INTEGER           4
#define SQL_SMALLINT          5
#define SQL_FLOAT             6
#define SQL_REAL              7
#define SQL_DOUBLE            8
#define SQL_TIMESTAMP        11
#define SQL_VARCHAR          12
#define SQL_NULLABLE_UNKNOWN  2
#define SQL_ALL_EXCEPT_LIKE   2
#define SQL_SEARCHABLE        3
#define SQL_ALL_TYPES         0
#define SQL_TABLE_STAT        0
#define SQL_BEST_ROWID        1
#define SQL_PC_NOT_PSEUDO     1
#define SQL_PC_PSEUDO         2
#define SQL_SCOPE_CURROW      0
#endif   // !WIN32

//typedef class INDEXDEF *PIXDEF;

/***********************************************************************/
/*  Defines the structure used to enumerate tables or views.           */
/***********************************************************************/
typedef struct _curtab {
  PRELDEF CurTdb;
  char   *Curp;
  char   *Tabpat;
  bool    Ispat;
  bool    NoView;
  int     Nt;
  char   *Type[16];
  } CURTAB, *PCURTAB;

/***********************************************************************/
/*  Defines the structure used to get column catalog info.             */
/***********************************************************************/
typedef struct _colinfo {
	char  *Name;
  int    Type;
  int    Offset;
  int    Length;
	int    Key;
	int    Prec;
	int    Opt;
	char  *Remark;
	char  *Datefmt;
  char  *Fieldfmt;
	ushort Flags;				 // Used by MariaDB CONNECT handlers
  } COLINFO, *PCOLINFO;

/***********************************************************************/
/*  CATALOG: base class for catalog classes.                           */
/***********************************************************************/
class DllExport CATALOG {
	friend class RELDEF;
	friend class TABDEF;
  friend class DIRDEF;
	friend class OEMDEF;
 public:
  CATALOG(void);                       // Constructor

  // Implementation
  void   *GetDescp(void) {return Descp;}
  PRELDEF GetTo_Desc(void) {return To_Desc;}
//PSZ     GetDescFile(void) {return DescFile;}
  int     GetCblen(void) {return Cblen;}
  bool    GetDefHuge(void) {return DefHuge;}
  void    SetDefHuge(bool b) {DefHuge = b;}
	bool    GetSepIndex(void) {return SepIndex;}
	void    SetSepIndex(bool b) {SepIndex = b;}
  char   *GetCbuf(void) {return Cbuf;}
  char   *GetDataPath(void) {return (char*)DataPath;}

  // Methods
  virtual void    Reset(void) {}
  virtual void    SetDataPath(PGLOBAL g, const char *path) {}
  virtual bool    GetBoolCatInfo(LPCSTR name, PSZ what, bool bdef) {return bdef;}
  virtual bool    SetIntCatInfo(LPCSTR name, PSZ what, int ival) {return false;}
  virtual int     GetIntCatInfo(LPCSTR name, PSZ what, int idef) {return idef;}
  virtual int     GetSizeCatInfo(LPCSTR name, PSZ what, PSZ sdef) {return 0;}
  virtual int     GetCharCatInfo(LPCSTR name, PSZ what, PSZ sdef, char *buf, int size)
																{strncpy(buf, sdef, size); return size;} 
  virtual char   *GetStringCatInfo(PGLOBAL g, PSZ name, PSZ what, PSZ sdef)
																{return sdef;}
	virtual int     GetColCatInfo(PGLOBAL g, PTABDEF defp) {return -1;}
	virtual bool		GetIndexInfo(PGLOBAL g, PTABDEF defp) {return true;}
  virtual bool    CheckName(PGLOBAL g, char *name) {return true;}
  virtual bool    ClearName(PGLOBAL g, PSZ name) {return true;}
  virtual PRELDEF MakeOneTableDesc(PGLOBAL g, LPCSTR name, LPCSTR am) {return NULL;}
  virtual PRELDEF GetTableDescEx(PGLOBAL g, PTABLE tablep) {return NULL;}
  virtual PRELDEF GetTableDesc(PGLOBAL g, LPCSTR name, LPCSTR am,
																					PRELDEF *prp = NULL) {return NULL;}
  virtual PRELDEF GetFirstTable(PGLOBAL g) {return NULL;}
  virtual PRELDEF GetNextTable(PGLOBAL g) {return NULL;}
  virtual bool    TestCond(PGLOBAL g, const char *name, const char *type) {return true;}
  virtual bool    DropTable(PGLOBAL g, PSZ name, bool erase) {return true;}
  virtual PTDB    GetTable(PGLOBAL g, PTABLE tablep, MODE mode = MODE_READ) {return NULL;}
  virtual void    TableNames(PGLOBAL g, char *buffer, int maxbuf, int info[]) {}
  virtual void    ColumnNames(PGLOBAL g, char *tabname, char *buffer,
                                         int maxbuf, int info[]) {}
  virtual void    ColumnDefs(PGLOBAL g, char *tabname, char *buffer,
                                        int maxbuf, int info[]) {}
  virtual void   *DecodeValues(PGLOBAL g, char *tabname, char *colname,
                                  char *buffer, int maxbuf, int info[]) {return NULL;}
  virtual int     ColumnType(PGLOBAL g, char *tabname, char *colname) {return 0;}
  virtual void    ClearDB(PGLOBAL g) {}

 protected:
  virtual bool    ClearSection(PGLOBAL g, const char *key, const char *section) {return true;}
  virtual PRELDEF MakeTableDesc(PGLOBAL g, LPCSTR name, LPCSTR am) {return NULL;}

  // Members
  PRELDEF To_Desc;                     /* To chain of relation desc.   */
  void   *Descp;                       /* To DB description area       */
//AREADEF DescArea;                    /* Table desc. area size        */
  char   *Cbuf;                        /* Buffer used for col section  */
  int     Cblen;                       /* Length of suballoc. buffer   */
  CURTAB  Ctb;                         /* Used to enumerate tables     */
  bool    DefHuge;                     /* true: tables default to huge */
	bool    SepIndex;                    /* true: separate index files   */
//char    DescFile[_MAX_PATH];         /* DB description filename      */
  LPCSTR  DataPath;                    /* Is the Path of DB data dir   */
  }; // end of class CATALOG

#endif // __CATALOG__H
