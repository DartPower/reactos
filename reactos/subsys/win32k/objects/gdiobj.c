/*
 *  ReactOS W32 Subsystem
 *  Copyright (C) 1998, 1999, 2000, 2001, 2002, 2003 ReactOS Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */
/*
 * GDIOBJ.C - GDI object manipulation routines
 *
 * $Id: gdiobj.c,v 1.38 2003/08/20 07:45:02 gvg Exp $
 *
 */

#undef WIN32_LEAN_AND_MEAN
#define WIN32_NO_STATUS
#include <windows.h>
#include <ddk/ntddk.h>
#include <include/dce.h>
#include <include/object.h>
#include <win32k/gdiobj.h>
#include <win32k/brush.h>
#include <win32k/pen.h>
#include <win32k/text.h>
#include <win32k/dc.h>
#include <win32k/bitmaps.h>
#include <win32k/region.h>
#include <win32k/cursoricon.h>
#include <include/palette.h>
#define NDEBUG
#include <win32k/debug1.h>

#define GDI_HANDLE_INDEX_MASK 0x00000fff
#define GDI_HANDLE_TYPE_MASK  0x007f0000
#define GDI_HANDLE_STOCK_MASK 0x00800000

#define GDI_HANDLE_CREATE(i, t)    ((HANDLE)(((i) & GDI_HANDLE_INDEX_MASK) | ((t) & GDI_HANDLE_TYPE_MASK)))
#define GDI_HANDLE_GET_INDEX(h)    (((DWORD)(h)) & GDI_HANDLE_INDEX_MASK)
#define GDI_HANDLE_GET_TYPE(h)     (((DWORD)(h)) & GDI_HANDLE_TYPE_MASK)
#define GDI_HANDLE_IS_TYPE(h, t)   ((t) == (((DWORD)(h)) & GDI_HANDLE_TYPE_MASK))
#define GDI_HANDLE_IS_STOCKOBJ(h)  (0 != (((DWORD)(h)) & GDI_HANDLE_STOCK_MASK))
#define GDI_HANDLE_SET_STOCKOBJ(h) ((h) = (HANDLE)(((DWORD)(h)) | GDI_HANDLE_STOCK_MASK))

/*  GDI stock objects */

static LOGBRUSH WhiteBrush =
{ BS_SOLID, RGB(255,255,255), 0 };

static LOGBRUSH LtGrayBrush =
/* FIXME : this should perhaps be BS_HATCHED, at least for 1 bitperpixel */
{ BS_SOLID, RGB(192,192,192), 0 };

static LOGBRUSH GrayBrush =
/* FIXME : this should perhaps be BS_HATCHED, at least for 1 bitperpixel */
{ BS_SOLID, RGB(128,128,128), 0 };

static LOGBRUSH DkGrayBrush =
/* This is BS_HATCHED, for 1 bitperpixel. This makes the spray work in pbrush */
/* NB_HATCH_STYLES is an index into HatchBrushes */
{ BS_HATCHED, RGB(0,0,0), NB_HATCH_STYLES };

static LOGBRUSH BlackBrush =
{ BS_SOLID, RGB(0,0,0), 0 };

static LOGBRUSH NullBrush =
{ BS_NULL, 0, 0 };

static LOGPEN WhitePen =
{ PS_SOLID, { 0, 0 }, RGB(255,255,255) };

static LOGPEN BlackPen =
{ PS_SOLID, { 0, 0 }, RGB(0,0,0) };

static LOGPEN NullPen =
{ PS_NULL, { 0, 0 }, 0 };

static LOGFONTW OEMFixedFont =
{ 14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, OEM_CHARSET,
  0, 0, DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, L"" };

static LOGFONTW AnsiFixedFont =
{ 14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, ANSI_CHARSET,
  0, 0, DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, L"" };

/*static LOGFONTW AnsiVarFont =
 *{ 14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, ANSI_CHARSET,
 *  0, 0, DEFAULT_QUALITY, VARIABLE_PITCH | FF_SWISS, L"MS Sans Serif" }; */

static LOGFONTW SystemFont =
{ 14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, ANSI_CHARSET,
  0, 0, DEFAULT_QUALITY, VARIABLE_PITCH | FF_SWISS, L"System" };

static LOGFONTW DeviceDefaultFont =
{ 14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, ANSI_CHARSET,
  0, 0, DEFAULT_QUALITY, VARIABLE_PITCH | FF_SWISS, L"" };

static LOGFONTW SystemFixedFont =
{ 14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, ANSI_CHARSET,
  0, 0, DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, L"" };

/* FIXME: Is this correct? */
static LOGFONTW DefaultGuiFont =
{ 14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, ANSI_CHARSET,
  0, 0, DEFAULT_QUALITY, VARIABLE_PITCH | FF_SWISS, L"MS Sans Serif" };

#define NB_STOCK_OBJECTS (DEFAULT_GUI_FONT + 1)

static HGDIOBJ *StockObjects[NB_STOCK_OBJECTS];
static PGDI_HANDLE_TABLE  HandleTable = 0;
static FAST_MUTEX  HandleTableMutex;
static FAST_MUTEX  RefCountHandling;

/*! Size of the GDI handle table
 * http://www.windevnet.com/documents/s=7290/wdj9902b/9902b.htm
 * gdi handle table can hold 0x4000 handles
*/
#define GDI_HANDLE_NUMBER  0x4000

/*!
 * Allocate GDI object table.
 * \param	Size - number of entries in the object table.
*/
static PGDI_HANDLE_TABLE FASTCALL
GDIOBJ_iAllocHandleTable (WORD Size)
{
  PGDI_HANDLE_TABLE  handleTable;

  ExAcquireFastMutexUnsafe (&HandleTableMutex);
  handleTable = ExAllocatePool(PagedPool,
                               sizeof (GDI_HANDLE_TABLE) +
                                 sizeof (GDI_HANDLE_ENTRY) * Size);
  ASSERT( handleTable );
  memset (handleTable,
          0,
          sizeof (GDI_HANDLE_TABLE) + sizeof (GDI_HANDLE_ENTRY) * Size);
  handleTable->wTableSize = Size;
  ExReleaseFastMutexUnsafe (&HandleTableMutex);

  return  handleTable;
}

/*!
 * Returns the entry into the handle table by index.
*/
static PGDI_HANDLE_ENTRY FASTCALL
GDIOBJ_iGetHandleEntryForIndex (WORD TableIndex)
{
  /*DPRINT("GDIOBJ_iGetHandleEntryForIndex: TableIndex: %d,\n handle: %x, ptr: %x\n", TableIndex, HandleTable->Handles [TableIndex], &(HandleTable->Handles [TableIndex])  );*/
  /*DPRINT("GIG: HandleTable: %x, Handles: %x, \n TableIndex: %x, pt: %x\n", HandleTable,  HandleTable->Handles, TableIndex, ((PGDI_HANDLE_ENTRY)HandleTable->Handles+TableIndex));*/
  /*DPRINT("GIG: Hndl: %x\n", ((PGDI_HANDLE_ENTRY)HandleTable->Handles+TableIndex));*/
  return  ((PGDI_HANDLE_ENTRY)HandleTable->Handles+TableIndex);
}

/*!
 * Finds next free entry in the GDI handle table.
 * \return	index into the table is successful, zero otherwise.
*/
static WORD FASTCALL
GDIOBJ_iGetNextOpenHandleIndex (void)
{
  WORD tableIndex;

  ExAcquireFastMutexUnsafe (&HandleTableMutex);
  for (tableIndex = 1; tableIndex < HandleTable->wTableSize; tableIndex++)
    {
      if (NULL == HandleTable->Handles[tableIndex].pObject)
	{
	  HandleTable->Handles[tableIndex].pObject = (PGDIOBJ) -1;
	  break;
	}
    }
  ExReleaseFastMutexUnsafe (&HandleTableMutex);

  return  (tableIndex < HandleTable->wTableSize) ? tableIndex : 0;
}

/*!
 * Allocate memory for GDI object and return handle to it.
 *
 * \param Size - size of the GDI object. This shouldn't to include the size of GDIOBJHDR.
 * The actual amount of allocated memory is sizeof(GDIOBJHDR)+Size
 * \param ObjectType - type of object \ref GDI object types
 * \param CleanupProcPtr - Routine to be called on destruction of object
 *
 * \return Handle of the allocated object.
 *
 * \note Use GDIOBJ_Lock() to obtain pointer to the new object.
*/
HGDIOBJ FASTCALL
GDIOBJ_AllocObj(WORD Size, DWORD ObjectType, GDICLEANUPPROC CleanupProc)
{
  PGDIOBJHDR  newObject;
  PGDI_HANDLE_ENTRY  handleEntry;

  DPRINT("GDIOBJ_AllocObj: size: %d, type: 0x%08x\n", Size, ObjectType);
  newObject = ExAllocatePool (PagedPool, Size + sizeof (GDIOBJHDR));
  if (newObject == NULL)
  {
    DPRINT("GDIOBJ_AllocObj: failed\n");
    return  NULL;
  }
  RtlZeroMemory (newObject, Size + sizeof (GDIOBJHDR));

  newObject->wTableIndex = GDIOBJ_iGetNextOpenHandleIndex ();
  newObject->dwCount = 0;
  handleEntry = GDIOBJ_iGetHandleEntryForIndex (newObject->wTableIndex);
  handleEntry->CleanupProc = CleanupProc;
  handleEntry->hProcessId = PsGetCurrentProcessId ();
  handleEntry->pObject = newObject;
  handleEntry->lockfile = NULL;
  handleEntry->lockline = 0;
  DPRINT("GDIOBJ_AllocObj: object handle %d\n", newObject->wTableIndex );
  return GDI_HANDLE_CREATE(newObject->wTableIndex, ObjectType);
}

/*!
 * Free memory allocated for the GDI object. For each object type this function calls the
 * appropriate cleanup routine.
 *
 * \param hObj       - handle of the object to be deleted.
 * \param ObjectType - one of the \ref GDI object types
 * or GDI_OBJECT_TYPE_DONTCARE.
 * \param Flag       - if set to GDIOBJFLAG_IGNOREPID then the routine doesn't check if the process that
 * tries to delete the object is the same one that created it.
 *
 * \return Returns TRUE if succesful.
 *
 * \note You should only use GDIOBJFLAG_IGNOREPID if you are cleaning up after the process that terminated.
 * \note This function deferres object deletion if it is still in use.
*/
BOOL STDCALL
GDIOBJ_FreeObj(HGDIOBJ hObj, DWORD ObjectType, DWORD Flag)
{
  PGDIOBJHDR objectHeader;
  PGDI_HANDLE_ENTRY handleEntry;
  PGDIOBJ Obj;
  BOOL 	bRet = TRUE;

  handleEntry = GDIOBJ_iGetHandleEntryForIndex(GDI_HANDLE_GET_INDEX(hObj));
  DPRINT("GDIOBJ_FreeObj: hObj: 0x%08x, handleEntry: %x\n", hObj, handleEntry );

  if (NULL == handleEntry
      || (GDI_HANDLE_GET_TYPE(hObj) != ObjectType && ObjectType != GDI_OBJECT_TYPE_DONTCARE)
      || ((handleEntry->hProcessId != PsGetCurrentProcessId()) && !(Flag & GDIOBJFLAG_IGNOREPID)))
    {
      DPRINT("Can't Delete hObj: 0x%08x, type: 0x%08x, pid:%d\n currpid:%d, flag:%d, hmm:%d\n", hObj, ObjectType, handleEntry->hProcessId, PsGetCurrentProcessId(), (Flag&GDIOBJFLAG_IGNOREPID), ((handleEntry->hProcessId != PsGetCurrentProcessId()) && !(Flag&GDIOBJFLAG_IGNOREPID)) );
      return FALSE;
    }

  objectHeader = handleEntry->pObject;
  ASSERT(objectHeader);
  DPRINT("FreeObj: locks: %x\n", objectHeader->dwCount );
  if (!(Flag & GDIOBJFLAG_IGNORELOCK))
    {
      /* check that the reference count is zero. if not then set flag
       * and delete object when releaseobj is called */
      ExAcquireFastMutex(&RefCountHandling);
      if ((objectHeader->dwCount & ~0x80000000) > 0 )
	{
	  DPRINT("GDIOBJ_FreeObj: delayed object deletion: count %d\n", objectHeader->dwCount);
	  objectHeader->dwCount |= 0x80000000;
	  ExReleaseFastMutex(&RefCountHandling);
	  return TRUE;
	}
      ExReleaseFastMutex(&RefCountHandling);
    }

  /* allow object to delete internal data */
  if (NULL != handleEntry->CleanupProc)
    {
      Obj = (PGDIOBJ)((PCHAR)handleEntry->pObject + sizeof(GDIOBJHDR));
      bRet = (*(handleEntry->CleanupProc))(Obj);
    }
  ExFreePool (handleEntry->pObject);
  memset(handleEntry, 0, sizeof(GDI_HANDLE_ENTRY));

  return bRet;
}

/*!
 * Lock multiple objects. Use this function when you need to lock multiple objects and some of them may be
 * duplicates. You should use this function to avoid trying to lock the same object twice!
 *
 * \param	pList 	pointer to the list that contains handles to the objects. You should set hObj and ObjectType fields.
 * \param	nObj	number of objects to lock
 * \return	for each entry in pList this function sets pObj field to point to the object.
 *
 * \note this function uses an O(n^2) algoritm because we shouldn't need to call it with more than 3 or 4 objects.
*/
BOOL FASTCALL
GDIOBJ_LockMultipleObj(PGDIMULTILOCK pList, INT nObj)
{
  INT i, j;
  ASSERT( pList );
  /* FIXME - check for "invalid" handles */
  /* go through the list checking for duplicate objects */
  for (i = 0; i < nObj; i++)
    {
      pList[i].pObj = NULL;
      for (j = 0; j < i; j++)
	{
	  if (pList[i].hObj == pList[j].hObj)
	    {
	      /* already locked, so just copy the pointer to the object */
	      pList[i].pObj = pList[j].pObj;
	      break;
	    }
	}

      if (NULL == pList[i].pObj)
	{
	  /* object hasn't been locked, so lock it. */
	  if (NULL != pList[i].hObj)
	    {
	      pList[i].pObj = GDIOBJ_LockObj(pList[i].hObj, pList[i].ObjectType);
	    }
	}
    }

  return TRUE;
}

/*!
 * Unlock multiple objects. Use this function when you need to unlock multiple objects and some of them may be
 * duplicates.
 *
 * \param	pList 	pointer to the list that contains handles to the objects. You should set hObj and ObjectType fields.
 * \param	nObj	number of objects to lock
 *
 * \note this function uses O(n^2) algoritm because we shouldn't need to call it with more than 3 or 4 objects.
*/
BOOL FASTCALL
GDIOBJ_UnlockMultipleObj(PGDIMULTILOCK pList, INT nObj)
{
  INT i, j;
  ASSERT(pList);

  /* go through the list checking for duplicate objects */
  for (i = 0; i < nObj; i++)
    {
      if (NULL != pList[i].pObj)
	{
	  for (j = i + 1; j < nObj; j++)
	    {
	      if ((pList[i].pObj == pList[j].pObj))
		{
		  /* set the pointer to zero for all duplicates */
		  pList[j].pObj = NULL;
		}
	    }
	  GDIOBJ_UnlockObj(pList[i].hObj, pList[i].ObjectType);
	  pList[i].pObj = NULL;
	}
    }

  return TRUE;
}

/*!
 * Marks the object as global. (Creator process ID is set to 0xFFFFFFFF). Global objects may be
 * accessed by any process.
 * \param 	ObjectHandle - handle of the object to make global.
 *
 * \note	Only stock objects should be marked global.
*/
VOID FASTCALL
GDIOBJ_MarkObjectGlobal(HGDIOBJ ObjectHandle)
{
  PGDI_HANDLE_ENTRY  handleEntry;

  if (NULL == ObjectHandle)
    {
      return;
    }

  handleEntry = GDIOBJ_iGetHandleEntryForIndex(GDI_HANDLE_GET_INDEX(ObjectHandle));
  if (NULL == handleEntry)
    {
      return;
    }

  handleEntry->hProcessId = (HANDLE)0xFFFFFFFF;
}

/*!
 * Get the type of the object.
 * \param 	ObjectHandle - handle of the object.
 * \return 	One of the \ref GDI object types
*/
DWORD FASTCALL
GDIOBJ_GetObjectType(HGDIOBJ ObjectHandle)
{
  return GDI_HANDLE_GET_TYPE(ObjectHandle);
}

/*!
 * Initialization of the GDI object engine.
*/
VOID FASTCALL
InitGdiObjectHandleTable (VOID)
{
  DPRINT ("InitGdiObjectHandleTable\n");
  ExInitializeFastMutex (&HandleTableMutex);
  ExInitializeFastMutex (&RefCountHandling);

  HandleTable = GDIOBJ_iAllocHandleTable (GDI_HANDLE_NUMBER);
  DPRINT("HandleTable: %x\n", HandleTable );

  InitEngHandleTable();
}

/*!
 * Creates a bunch of stock objects: brushes, pens, fonts.
*/
VOID FASTCALL
CreateStockObjects(void)
{
  unsigned Object;

  /* Create GDI Stock Objects from the logical structures we've defined */

  StockObjects[WHITE_BRUSH] =  NtGdiCreateBrushIndirect(&WhiteBrush);
  StockObjects[LTGRAY_BRUSH] = NtGdiCreateBrushIndirect(&LtGrayBrush);
  StockObjects[GRAY_BRUSH] =   NtGdiCreateBrushIndirect(&GrayBrush);
  StockObjects[DKGRAY_BRUSH] = NtGdiCreateBrushIndirect(&DkGrayBrush);
  StockObjects[BLACK_BRUSH] =  NtGdiCreateBrushIndirect(&BlackBrush);
  StockObjects[NULL_BRUSH] =   NtGdiCreateBrushIndirect(&NullBrush);

  StockObjects[WHITE_PEN] = NtGdiCreatePenIndirect(&WhitePen);
  StockObjects[BLACK_PEN] = NtGdiCreatePenIndirect(&BlackPen);
  StockObjects[NULL_PEN] =  NtGdiCreatePenIndirect(&NullPen);

  (void) TextIntCreateFontIndirect(&OEMFixedFont, (HFONT*)&StockObjects[OEM_FIXED_FONT]);
  (void) TextIntCreateFontIndirect(&AnsiFixedFont, (HFONT*)&StockObjects[ANSI_FIXED_FONT]);
  (void) TextIntCreateFontIndirect(&SystemFont, (HFONT*)&StockObjects[SYSTEM_FONT]);
  (void) TextIntCreateFontIndirect(&DeviceDefaultFont, (HFONT*)&StockObjects[DEVICE_DEFAULT_FONT]);
  (void) TextIntCreateFontIndirect(&SystemFixedFont, (HFONT*)&StockObjects[SYSTEM_FIXED_FONT]);
  (void) TextIntCreateFontIndirect(&DefaultGuiFont, (HFONT*)&StockObjects[DEFAULT_GUI_FONT]);

  StockObjects[DEFAULT_PALETTE] = (HGDIOBJ*)PALETTE_Init();

  for (Object = 0; Object < NB_STOCK_OBJECTS; Object++)
    {
      GDIOBJ_MarkObjectGlobal(StockObjects[Object]);
      GDI_HANDLE_SET_STOCKOBJ(StockObjects[Object]);
    }
}

/*!
 * Return stock object.
 * \param	Object - stock object id.
 * \return	Handle to the object.
*/
HGDIOBJ STDCALL
NtGdiGetStockObject(INT Object)
{
  return ((Object < 0) || (NB_STOCK_OBJECTS <= Object)) ? NULL : StockObjects[Object];
}

/*!
 * Delete GDI object
 * \param	hObject object handle
 * \return	if the function fails the returned value is NULL.
*/
BOOL STDCALL
NtGdiDeleteObject(HGDIOBJ hObject)
{
  return GDIOBJ_FreeObj(hObject, GDI_OBJECT_TYPE_DONTCARE, GDIOBJFLAG_DEFAULT);
}

/*!
 * Internal function. Called when the process is destroyed to free the remaining GDI handles.
 * \param	Process - PID of the process that will be destroyed.
*/
BOOL FASTCALL
CleanupForProcess (struct _EPROCESS *Process, INT Pid)
{
  DWORD i;
  PGDI_HANDLE_ENTRY handleEntry;
  PGDIOBJHDR objectHeader;
  PEPROCESS CurrentProcess;

  CurrentProcess = PsGetCurrentProcess();
  if (CurrentProcess != Process)
    {
      KeAttachProcess(Process);
    }

  for(i = 1; i < GDI_HANDLE_NUMBER; i++)
    {
      handleEntry = GDIOBJ_iGetHandleEntryForIndex(i);
      if (NULL != handleEntry && NULL != handleEntry->pObject &&
          (INT)handleEntry->hProcessId == Pid)
	{
	  objectHeader = (PGDIOBJHDR) handleEntry->pObject;
	  DPRINT("\nNtGdiCleanup: %d, process: %d, locks: %d", i, handleEntry->hProcessId, objectHeader->dwCount);
	  GDIOBJ_FreeObj(GDI_HANDLE_CREATE(i, GDI_OBJECT_TYPE_DONTCARE),
	                 GDI_OBJECT_TYPE_DONTCARE,
	                 GDIOBJFLAG_IGNOREPID|GDIOBJFLAG_IGNORELOCK );
	}
    }

  if (CurrentProcess != Process)
    {
      KeDetachProcess();
    }

  return TRUE;
}

#define GDIOBJ_TRACKLOCKS

#ifdef GDIOBJ_LockObj
#undef GDIOBJ_LockObj
PGDIOBJ FASTCALL
GDIOBJ_LockObjDbg (const char* file, int line, HGDIOBJ hObj, DWORD ObjectType)
{
  PGDIOBJ rc;
  PGDI_HANDLE_ENTRY handleEntry
    = GDIOBJ_iGetHandleEntryForIndex(GDI_HANDLE_GET_INDEX(hObj));
  if (NULL == handleEntry
      || (GDI_HANDLE_GET_TYPE(hObj) != ObjectType && ObjectType != GDI_OBJECT_TYPE_DONTCARE)
      || (handleEntry->hProcessId != (HANDLE)0xFFFFFFFF
	  && handleEntry->hProcessId != PsGetCurrentProcessId ()
	 )
     )
    {
      int reason = 0;
      if (NULL == handleEntry)
	{
	  reason = 1;
	}
      else if (GDI_HANDLE_GET_TYPE(hObj) != ObjectType && ObjectType != GDI_OBJECT_TYPE_DONTCARE)
	{
	  reason = 2;
	}
      else if (handleEntry->hProcessId != (HANDLE)0xFFFFFFFF
	   && handleEntry->hProcessId != PsGetCurrentProcessId())
	{
	  reason = 3;
	}

      DPRINT1("GDIOBJ_LockObj failed for 0x%08x, reqtype 0x%08x reason %d\n",
              hObj, ObjectType, reason );
      DPRINT1("\tcalled from: %s:%i\n", file, line );
      return NULL;
    }
  if (NULL != handleEntry->lockfile)
    {
      DPRINT1("Caution! GDIOBJ_LockObj trying to lock object (0x%x) second time\n", hObj );
      DPRINT1("\tcalled from: %s:%i\n", file, line );
      DPRINT1("\tpreviously locked from: %s:%i\n", handleEntry->lockfile, handleEntry->lockline );
    }
  DPRINT("(%s:%i) GDIOBJ_LockObj(0x%08x,0x%08x)\n", file, line, hObj, ObjectType);
  rc = GDIOBJ_LockObj(hObj, ObjectType);
  if (rc && NULL == handleEntry->lockfile)
    {
      handleEntry->lockfile = file;
      handleEntry->lockline = line;
    }

  return rc;
}
#endif//GDIOBJ_LockObj

#ifdef GDIOBJ_UnlockObj
#undef GDIOBJ_UnlockObj
BOOL FASTCALL
GDIOBJ_UnlockObjDbg (const char* file, int line, HGDIOBJ hObj, DWORD ObjectType)
{
  PGDI_HANDLE_ENTRY handleEntry
    = GDIOBJ_iGetHandleEntryForIndex (GDI_HANDLE_GET_INDEX(hObj));
  if (NULL == handleEntry
      || (GDI_HANDLE_GET_TYPE(hObj) != ObjectType && ObjectType != GDI_OBJECT_TYPE_DONTCARE)
      || (handleEntry->hProcessId != (HANDLE)0xFFFFFFFF
          && handleEntry->hProcessId != PsGetCurrentProcessId ()
         )
     )
    {
      DPRINT1("GDIBOJ_UnlockObj failed for 0x%08x, reqtype 0x%08x\n",
		  hObj, ObjectType);
      DPRINT1("\tcalled from: %s:%i\n", file, line);
      return FALSE;
    }
  DPRINT("(%s:%i) GDIOBJ_UnlockObj(0x%08x,0x%08x)\n", file, line, hObj, ObjectType);
  handleEntry->lockfile = NULL;
  handleEntry->lockline = 0;
  return GDIOBJ_UnlockObj(hObj, ObjectType);
}
#endif//GDIOBJ_LockObj

/*!
 * Return pointer to the object by handle.
 *
 * \param hObj 		Object handle
 * \param ObjectType	one of the object types defined in \ref GDI object types
 * \return		Pointer to the object.
 *
 * \note Process can only get pointer to the objects it created or global objects.
 *
 * \todo Don't allow to lock the objects twice! Synchronization!
*/
PGDIOBJ FASTCALL
GDIOBJ_LockObj(HGDIOBJ hObj, DWORD ObjectType)
{
  PGDI_HANDLE_ENTRY handleEntry
    = GDIOBJ_iGetHandleEntryForIndex(GDI_HANDLE_GET_INDEX(hObj));
  PGDIOBJHDR  objectHeader;

  DPRINT("GDIOBJ_LockObj: hObj: 0x%08x, type: 0x%08x, handleEntry: %x\n", hObj, ObjectType, handleEntry);
  if (NULL == handleEntry
      || (GDI_HANDLE_GET_TYPE(hObj) != ObjectType && ObjectType != GDI_OBJECT_TYPE_DONTCARE)
      || (handleEntry->hProcessId != (HANDLE)0xFFFFFFFF
          && handleEntry->hProcessId != PsGetCurrentProcessId()
	 )
     )
    {
      DPRINT1("GDIBOJ_LockObj failed for 0x%08x, type 0x%08x\n",
		  hObj, ObjectType);
      return NULL;
    }

  objectHeader = (PGDIOBJHDR) handleEntry->pObject;
  ASSERT(objectHeader);
  if(0 < objectHeader->dwCount)
    {
      DPRINT1("Caution! GDIOBJ_LockObj trying to lock object (0x%x) second time\n", hObj);
      DPRINT1("\t called from: %x\n", __builtin_return_address(0));
    }

  ExAcquireFastMutex(&RefCountHandling);
  objectHeader->dwCount++;
  ExReleaseFastMutex(&RefCountHandling);
  return (PGDIOBJ)((PCHAR)objectHeader + sizeof(GDIOBJHDR));
}

/*!
 * Release GDI object. Every object locked by GDIOBJ_LockObj() must be unlocked. You should unlock the object
 * as soon as you don't need to have access to it's data.

 * \param hObj 		Object handle
 * \param ObjectType	one of the object types defined in \ref GDI object types
 *
 * \note This function performs delayed cleanup. If the object is locked when GDI_FreeObj() is called
 * then \em this function frees the object when reference count is zero.
 *
 * \todo Change synchronization algorithm.
*/
#undef GDIOBJ_UnlockObj
BOOL FASTCALL
GDIOBJ_UnlockObj(HGDIOBJ hObj, DWORD ObjectType)
{
  PGDI_HANDLE_ENTRY handleEntry
    = GDIOBJ_iGetHandleEntryForIndex(GDI_HANDLE_GET_INDEX(hObj));
  PGDIOBJHDR  objectHeader;

  DPRINT("GDIOBJ_UnlockObj: hObj: 0x%08x, type: 0x%08x, handleEntry: %x\n", hObj, ObjectType, handleEntry);
  if (NULL == handleEntry
      || (GDI_HANDLE_GET_TYPE(hObj) != ObjectType && ObjectType != GDI_OBJECT_TYPE_DONTCARE)
      || (handleEntry->hProcessId != (HANDLE)0xFFFFFFFF
          && handleEntry->hProcessId != PsGetCurrentProcessId ()
	 )
     )
  {
    DPRINT1( "GDIOBJ_UnLockObj: failed\n");
    return FALSE;
  }

  objectHeader = (PGDIOBJHDR) handleEntry->pObject;
  ASSERT(objectHeader);

  ExAcquireFastMutex(&RefCountHandling);
  if (0 == (objectHeader->dwCount & ~0x80000000))
    {
      ExReleaseFastMutex(&RefCountHandling);
      DPRINT1( "GDIOBJ_UnLockObj: unlock object (0x%x) that is not locked\n", hObj );
      return FALSE;
    }

  objectHeader->dwCount--;

  if( objectHeader->dwCount == 0x80000000 )
    {
      //delayed object release
      objectHeader->dwCount = 0;
      ExReleaseFastMutex(&RefCountHandling);
      DPRINT("GDIOBJ_UnlockObj: delayed delete\n");
      return GDIOBJ_FreeObj(hObj, ObjectType, GDIOBJFLAG_DEFAULT);
    }
  ExReleaseFastMutex(&RefCountHandling);

  return TRUE;
}

/* EOF */
