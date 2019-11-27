/*
Copyright 2006 - 2018 Intel Corporation

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

	http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#include <assert.h>
#ifdef MEMORY_CHECK
#define MEMCHECK(x) x
#else
#define MEMCHECK(x)
#endif

#if defined(WIN32) && !defined(_WIN32_WCE)
#define _CRTDBG_MAP_ALLOC
#include <crtdbg.h>
#endif


#include "ILibParsers.h"
#include "ILibRemoteLogging.h"
#include "ILibProcessPipe.h"
#ifndef WIN32
#include <fcntl.h>              /* Obtain O_* constant definitions */
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#if !defined( __APPLE__) && !defined(_FREEBSD)
#include <pty.h>
#endif
#endif

#define CONSOLE_SCREEN_WIDTH 80
#define CONSOLE_SCREEN_HEIGHT 25

typedef struct ILibProcessPipe_Manager_Object
{
	ILibChain_Link ChainLink;
	ILibLinkedList ActivePipes;
}ILibProcessPipe_Manager_Object;
struct ILibProcessPipe_PipeObject;

typedef void(*ILibProcessPipe_GenericReadHandler)(char *buffer, int bufferLen, int* bytesConsumed, void* user1, void* user2);
typedef void(*ILibProcessPipe_GenericSendOKHandler)(void* user1, void* user2);
typedef void(*ILibProcessPipe_GenericBrokenPipeHandler)(struct ILibProcessPipe_PipeObject* sender);
struct ILibProcessPipe_Process_Object; // Forward Prototype

typedef struct ILibProcessPipe_PipeObject
{
	char* buffer;
	int bufferSize;

	int readOffset, readNewOffset;
	int totalRead;
	int processingLoop;

	ILibProcessPipe_Manager_Object *manager;
	struct ILibProcessPipe_Process_Object* mProcess;
	ILibQueue WriteBuffer;
	void *handler;
	ILibProcessPipe_GenericBrokenPipeHandler brokenPipeHandler;
	void *user1, *user2;
#ifdef WIN32
	int usingCompletionRoutine;
	HANDLE mPipe_Reader_ResumeEvent;
	HANDLE mPipe_ReadEnd;
	HANDLE mPipe_WriteEnd;
	OVERLAPPED *mOverlapped,*mwOverlapped;
	void *user3, *user4;
#else
	int mPipe_ReadEnd, mPipe_WriteEnd;
#endif
	int PAUSED;
}ILibProcessPipe_PipeObject;



typedef struct ILibProcessPipe_Process_Object
{
	int exiting;
	unsigned int flags1, flags2;
	ILibProcessPipe_Manager_Object *parent;
#ifdef WIN32
	DWORD PID;
#else
	pid_t PID;
#endif
	void *userObject;
	
	ILibProcessPipe_PipeObject *stdIn;
	ILibProcessPipe_PipeObject *stdOut;
	ILibProcessPipe_PipeObject *stdErr;
	ILibProcessPipe_Process_ExitHandler exitHandler;
#ifdef WIN32
	HANDLE hProcess;
	int hProcess_needAdd;
#endif
	void *chain;
}ILibProcessPipe_Process_Object;

typedef struct ILibProcessPipe_WriteData
{
	char *buffer;
	int bufferLen;
	ILibTransport_MemoryOwnership ownership;
}ILibProcessPipe_WriteData;

ILibProcessPipe_WriteData* ILibProcessPipe_WriteData_Create(char* buffer, int bufferLen, ILibTransport_MemoryOwnership ownership)
{
	ILibProcessPipe_WriteData* retVal;

	if ((retVal = (ILibProcessPipe_WriteData*)malloc(sizeof(ILibProcessPipe_WriteData))) == NULL) { ILIBCRITICALEXIT(254); }
	memset(retVal, 0, sizeof(ILibProcessPipe_WriteData));
	retVal->bufferLen = bufferLen;
	if (ownership == ILibTransport_MemoryOwnership_USER)
	{
		if ((retVal->buffer = (char*)malloc(bufferLen)) == NULL) { ILIBCRITICALEXIT(254); }
		memcpy_s(retVal->buffer, bufferLen, buffer, bufferLen);
		retVal->ownership = ILibTransport_MemoryOwnership_CHAIN;
	}
	else
	{
		retVal->buffer = buffer;
		retVal->ownership = ownership;
	}
	return retVal;
}
#define ILibProcessPipe_WriteData_Destroy(writeData) if (writeData->ownership == ILibTransport_MemoryOwnership_CHAIN) { free(writeData->buffer); } free(writeData);
ILibProcessPipe_Pipe ILibProcessPipe_Process_GetStdErr(ILibProcessPipe_Process p)
{
	return(((ILibProcessPipe_Process_Object*)p)->stdErr);
}
ILibProcessPipe_Pipe ILibProcessPipe_Process_GetStdOut(ILibProcessPipe_Process p)
{
	return(((ILibProcessPipe_Process_Object*)p)->stdOut);
}

#ifdef WIN32
BOOL ILibProcessPipe_Process_OnExit(HANDLE event, ILibWaitHandle_ErrorStatus errors, void* user);
typedef struct ILibProcessPipe_WaitHandle
{
	ILibProcessPipe_Manager_Object *parent;
	HANDLE event, registeredHandle;
	void *user;
	ILibProcessPipe_WaitHandle_Handler callback;
	int timeRemaining;
	int timeout;
	int contextSwitch;
}ILibProcessPipe_WaitHandle;
typedef struct ILibProcessPipe_WaitHandle_APC
{
	HANDLE callingThread;
	HANDLE ev;
	ILibWaitHandle_ErrorStatus status;
	ILibProcessPipe_WaitHandle_Handler callback;
	void *user;
}ILibProcessPipe_WaitHandle_APC;

void __stdcall ILibProcessPipe_WaitHandle_SignaledOrTimeout(void *user, BOOLEAN TimerOrWaitFired); // Prototype
int ILibProcessPipe_Manager_WindowsWaitHandles_Remove_event_Comparer(void *source, void *matchWith)
{
	if (source == NULL) { return 1; }
	return(((ILibProcessPipe_WaitHandle*)source)->event == matchWith ? 0 : 1);
}
int ILibProcessPipe_Manager_WindowsWaitHandles_Remove_registeredHandle_Comparer(void *source, void *matchWith)
{
	if (source == NULL) { return 1; }
	return(((ILibProcessPipe_WaitHandle*)source)->registeredHandle == matchWith ? 0 : 1);
}
void __stdcall ILibProcessPipe_WaitHandle_Remove_APC(ULONG_PTR obj)
{
	ILibProcessPipe_Manager_Object *manager = (ILibProcessPipe_Manager_Object*)((void**)obj)[0];
	HANDLE event = (HANDLE)((void**)obj)[1];
	void *node = ILibLinkedList_GetNode_Search(manager->ActivePipes, ILibProcessPipe_Manager_WindowsWaitHandles_Remove_event_Comparer, event);
	ILibProcessPipe_WaitHandle *waiter;

	if (node != NULL) 
	{ 
		waiter = (ILibProcessPipe_WaitHandle*)ILibLinkedList_GetDataFromNode(node);
		if (waiter->registeredHandle != NULL)
		{
			UnregisterWait(waiter->registeredHandle); waiter->registeredHandle = NULL;
		}
		ILibMemory_Free(waiter);
		ILibLinkedList_Remove(node);
	}
	free((void*)obj);
}
void ILibProcessPipe_WaitHandle_Remove(ILibProcessPipe_Manager mgr, HANDLE event)
{
	ILibProcessPipe_Manager_Object *manager = (ILibProcessPipe_Manager_Object*)mgr;
	void **data = (void**)ILibMemory_Allocate(2 * sizeof(void*), 0, NULL, NULL);
	data[0] = manager;
	data[1] = event;
	
	QueueUserAPC((PAPCFUNC)ILibProcessPipe_WaitHandle_Remove_APC, ILibChain_GetMicrostackThreadHandle(manager->ChainLink.ParentChain), (ULONG_PTR)data);
}
void __stdcall ILibProcessPipe_WaitHandle_unregister(ULONG_PTR u)
{
	ILibProcessPipe_WaitHandle *waitHandle = (ILibProcessPipe_WaitHandle*)u;
	if (!ILibMemory_CanaryOK((void*)u) || waitHandle->registeredHandle == NULL) { return; }
	void *node = ILibLinkedList_GetNode_Search(waitHandle->parent->ActivePipes, ILibProcessPipe_Manager_WindowsWaitHandles_Remove_registeredHandle_Comparer, waitHandle->registeredHandle);

	if (node != NULL)
	{
		ILibLinkedList_Remove(node);
	}

	UnregisterWait(waitHandle->registeredHandle);
	waitHandle->registeredHandle = NULL;
	ILibMemory_Free(waitHandle);
}
void __stdcall ILibProcessPipe_WaitHandle_reregister(ULONG_PTR u)
{
	if (!ILibMemory_CanaryOK((void*)u)) { return; }
	ILibProcessPipe_WaitHandle *waitHandle = (ILibProcessPipe_WaitHandle*)u;

	if (waitHandle->registeredHandle != NULL)
	{
		UnregisterWait(waitHandle->registeredHandle);
		waitHandle->registeredHandle = NULL;
	}
	RegisterWaitForSingleObject(&(waitHandle->registeredHandle), waitHandle->event, ILibProcessPipe_WaitHandle_SignaledOrTimeout, waitHandle, (ULONG)waitHandle->timeout, WT_EXECUTEINPERSISTENTTHREAD | WT_EXECUTEONLYONCE);
}
void __stdcall ILibProcessPipe_WaitHandle_SignaledOrTimeout(void *user, BOOLEAN TimerOrWaitFired)
{
	if (!ILibMemory_CanaryOK(user)) { return; }
	ILibProcessPipe_WaitHandle *waitHandle = (ILibProcessPipe_WaitHandle*)user;
	HANDLE chainHandle = ILibChain_GetMicrostackThreadHandle(waitHandle->parent->ChainLink.ParentChain);

	if (waitHandle->callback == NULL || waitHandle->callback(waitHandle->event, TimerOrWaitFired? ILibWaitHandle_ErrorStatus_TIMEOUT:ILibWaitHandle_ErrorStatus_NONE, waitHandle->user) == FALSE)
	{
		// Unregister
		QueueUserAPC((PAPCFUNC)ILibProcessPipe_WaitHandle_unregister, chainHandle, (ULONG_PTR)waitHandle);
	}
	else
	{
		// Re-Register
		QueueUserAPC((PAPCFUNC)ILibProcessPipe_WaitHandle_reregister, chainHandle, (ULONG_PTR)waitHandle);
	}
}
void ILibProcessPipe_WaitHandle_Add_WithNonZeroTimeout(ILibProcessPipe_Manager mgr, HANDLE event, int milliseconds, void *user, ILibProcessPipe_WaitHandle_Handler callback)
{
	ILibProcessPipe_Manager_Object *manager = (ILibProcessPipe_Manager_Object*)mgr;
	ILibProcessPipe_WaitHandle *waitHandle;
	waitHandle = (ILibProcessPipe_WaitHandle*)ILibMemory_SmartAllocate(sizeof(ILibProcessPipe_WaitHandle));

	waitHandle->parent = manager;
	waitHandle->event = event;
	waitHandle->user = user;
	waitHandle->callback = callback;
	waitHandle->timeout = milliseconds;

	if (RegisterWaitForSingleObject(&(waitHandle->registeredHandle), waitHandle->event, ILibProcessPipe_WaitHandle_SignaledOrTimeout, waitHandle, (ULONG)milliseconds, WT_EXECUTEINPERSISTENTTHREAD | WT_EXECUTEONLYONCE) == 0)
	{
		// FAILED
		ILibMemory_Free(waitHandle);
	}
	else
	{
		ILibLinkedList_AddTail(manager->ActivePipes, waitHandle);
	}
}
void ILibProcessPipe_WaitHandle_Add2_WithNonZeroTimeout(ILibProcessPipe_Manager mgr, HANDLE event, int milliseconds, void *user, ILibProcessPipe_WaitHandle_Handler callback)
{
	ILibProcessPipe_Manager_Object *manager = (ILibProcessPipe_Manager_Object*)mgr;
	ILibProcessPipe_WaitHandle *waitHandle;
	waitHandle = (ILibProcessPipe_WaitHandle*)ILibMemory_SmartAllocate(sizeof(ILibProcessPipe_WaitHandle));

	waitHandle->parent = manager;
	waitHandle->event = event;
	waitHandle->user = user;
	waitHandle->callback = callback;
	waitHandle->timeout = milliseconds;
	waitHandle->contextSwitch = 1;

	if (RegisterWaitForSingleObject(&(waitHandle->registeredHandle), waitHandle->event, ILibProcessPipe_WaitHandle_SignaledOrTimeout, waitHandle, (ULONG)milliseconds, WT_EXECUTEINPERSISTENTTHREAD | WT_EXECUTEONLYONCE) == 0)
	{
		// FAILED
		ILibMemory_Free(waitHandle);
	}
	else
	{
		ILibLinkedList_AddTail(manager->ActivePipes, waitHandle);
	}
}
#else
void ILibProcessPipe_Process_ReadHandler(void* user);
void ILibProcessPipe_Manager_OnPreSelect(void* object, fd_set *readset, fd_set *writeset, fd_set *errorset, int* blocktime)
{
	ILibProcessPipe_Manager_Object *man = (ILibProcessPipe_Manager_Object*)object;
	void *node, *nextnode;
	ILibProcessPipe_PipeObject *j;

	node = ILibLinkedList_GetNode_Head(man->ActivePipes);
	while(node != NULL && (j = (ILibProcessPipe_PipeObject*)ILibLinkedList_GetDataFromNode(node)) != NULL)
	{
		nextnode = ILibLinkedList_GetNextNode(node);
		if (((int*)ILibLinkedList_GetExtendedMemory(node))[0] != 0 || (j = (ILibProcessPipe_PipeObject*)ILibLinkedList_GetDataFromNode(node)) == NULL)
		{
			ILibLinkedList_Remove(node);
			node = nextnode;
			continue;
		}
		if (ILibMemory_CanaryOK(j) && j->mPipe_ReadEnd != -1)
		{
			FD_SET(j->mPipe_ReadEnd, readset);
		}
		node = nextnode;
	}
}
void ILibProcessPipe_Manager_OnPostSelect(void* object, int slct, fd_set *readset, fd_set *writeset, fd_set *errorset)
{
	ILibProcessPipe_Manager_Object *man = (ILibProcessPipe_Manager_Object*)object;
	void *node, *nextNode;
	ILibProcessPipe_PipeObject *j;

	node = ILibLinkedList_GetNode_Head(man->ActivePipes);
	while(node != NULL && (j = (ILibProcessPipe_PipeObject*)ILibLinkedList_GetDataFromNode(node)) != NULL)
	{
		nextNode = ILibLinkedList_GetNextNode(node);
		if (ILibMemory_CanaryOK(node) && ILibMemory_CanaryOK(j))
		{
			if (j->mPipe_ReadEnd != -1 && FD_ISSET(j->mPipe_ReadEnd, readset) != 0)
			{
				ILibProcessPipe_Process_ReadHandler(j);
			}
		}
		if (ILibChain_GetContinuationState(man->ChainLink.ParentChain) == ILibChain_ContinuationState_END_CONTINUE) { break; }
		node = nextNode;
	}
}
#endif
void ILibProcessPipe_Manager_OnDestroy(void *object)
{
	ILibProcessPipe_Manager_Object *man = (ILibProcessPipe_Manager_Object*)object;
	
#ifdef WIN32
	// ToDo: Enumerate List, and unregister everything
#endif
	ILibLinkedList_Destroy(man->ActivePipes);
}
ILibProcessPipe_Manager ILibProcessPipe_Manager_Create(void *chain)
{
	ILibProcessPipe_Manager_Object *retVal;

	if ((retVal = (ILibProcessPipe_Manager_Object*)malloc(sizeof(ILibProcessPipe_Manager_Object))) == NULL) { ILIBCRITICALEXIT(254); }
	memset(retVal, 0, sizeof(ILibProcessPipe_Manager_Object));
	retVal->ChainLink.MetaData = "ILibProcessPipe_Manager";
	retVal->ChainLink.ParentChain = chain;
	retVal->ActivePipes = ILibLinkedList_CreateEx(sizeof(int));

#ifndef WIN32
	retVal->ChainLink.PreSelectHandler = &ILibProcessPipe_Manager_OnPreSelect;
	retVal->ChainLink.PostSelectHandler = &ILibProcessPipe_Manager_OnPostSelect;
#endif
	retVal->ChainLink.DestroyHandler = &ILibProcessPipe_Manager_OnDestroy;

	if (ILibIsChainRunning(chain) == 0)
	{
		ILibAddToChain(chain, retVal);
	}
	else
	{
		ILibChain_SafeAdd(chain, retVal);
	}
	return retVal;
}

void ILibProcessPipe_FreePipe(ILibProcessPipe_PipeObject *pipeObject)
{
	if (!ILibMemory_CanaryOK(pipeObject)) { return; }
#ifdef WIN32
	if (pipeObject->mPipe_ReadEnd != NULL) { CloseHandle(pipeObject->mPipe_ReadEnd); }
	if (pipeObject->mPipe_WriteEnd != NULL && pipeObject->mPipe_WriteEnd != pipeObject->mPipe_ReadEnd) { CloseHandle(pipeObject->mPipe_WriteEnd); }
	if (pipeObject->mOverlapped != NULL) { CloseHandle(pipeObject->mOverlapped->hEvent); free(pipeObject->mOverlapped); }
	if (pipeObject->mwOverlapped != NULL) { free(pipeObject->mwOverlapped); }
	if (pipeObject->mPipe_Reader_ResumeEvent != NULL) { CloseHandle(pipeObject->mPipe_Reader_ResumeEvent); }
	if (pipeObject->buffer != NULL && pipeObject->usingCompletionRoutine == 0) { free(pipeObject->buffer); }
#else
	if (pipeObject->manager != NULL)
	{
		void *node = ILibLinkedList_GetNode_Search(pipeObject->manager->ActivePipes, NULL, pipeObject);
		if (node != NULL)
		{
			ILibLinkedList_Remove(node);
		}
	}
	if (pipeObject->mPipe_ReadEnd != -1) { close(pipeObject->mPipe_ReadEnd); }
	if (pipeObject->mPipe_WriteEnd != -1 && pipeObject->mPipe_WriteEnd != pipeObject->mPipe_ReadEnd) { close(pipeObject->mPipe_WriteEnd); }
	if (pipeObject->buffer != NULL) { free(pipeObject->buffer); }
#endif

	if (pipeObject->WriteBuffer != NULL)
	{
		ILibProcessPipe_WriteData* data;
		while ((data = (ILibProcessPipe_WriteData*)ILibQueue_DeQueue(pipeObject->WriteBuffer)) != NULL)
		{
			ILibProcessPipe_WriteData_Destroy(data);
		}
		ILibQueue_Destroy(pipeObject->WriteBuffer);
	}
	if (pipeObject->mProcess != NULL)
	{
		if (pipeObject->mProcess->stdIn == pipeObject) { pipeObject->mProcess->stdIn = NULL; }
		if (pipeObject->mProcess->stdOut == pipeObject) { pipeObject->mProcess->stdOut = NULL; }
		if (pipeObject->mProcess->stdErr == pipeObject) { pipeObject->mProcess->stdErr = NULL; }
	}
	ILibMemory_Free(pipeObject);
}

#ifdef WIN32
void ILibProcessPipe_PipeObject_DisableInherit(HANDLE* h)
{
	HANDLE tmpRead = *h;
	DuplicateHandle(GetCurrentProcess(), tmpRead, GetCurrentProcess(), h,  0, FALSE, DUPLICATE_SAME_ACCESS);
	CloseHandle(tmpRead);
}
#endif

#ifdef WIN32
ILibProcessPipe_Pipe ILibProcessPipe_Pipe_CreateFromExistingWithExtraMemory(ILibProcessPipe_Manager manager, HANDLE existingPipe, ILibProcessPipe_Pipe_ReaderHandleType handleType, int extraMemorySize)
#else
ILibProcessPipe_Pipe ILibProcessPipe_Pipe_CreateFromExistingWithExtraMemory(ILibProcessPipe_Manager manager, int existingPipe, int extraMemorySize)
#endif
{
	ILibProcessPipe_PipeObject* retVal = NULL;

	retVal = ILibMemory_SmartAllocateEx(sizeof(ILibProcessPipe_PipeObject), extraMemorySize);
	retVal->manager = (ILibProcessPipe_Manager_Object*)manager;

#ifdef WIN32
	if (handleType == ILibProcessPipe_Pipe_ReaderHandleType_Overlapped)
	{
		void *tmpExtra;
		retVal->mOverlapped = (OVERLAPPED*)ILibMemory_Allocate(sizeof(OVERLAPPED), sizeof(void*), NULL, &tmpExtra);
		if ((retVal->mOverlapped->hEvent = CreateEvent(NULL, TRUE, FALSE, NULL)) == NULL) { ILIBCRITICALEXIT(254); }
		((void**)tmpExtra)[0] = retVal;
	}
#else
	fcntl(existingPipe, F_SETFL, O_NONBLOCK);
#endif

	retVal->mPipe_ReadEnd = existingPipe;
	retVal->mPipe_WriteEnd = existingPipe;
	return retVal;
}

void ILibProcessPipe_Pipe_SetBrokenPipeHandler(ILibProcessPipe_Pipe targetPipe, ILibProcessPipe_Pipe_BrokenPipeHandler handler)
{
	((ILibProcessPipe_PipeObject*)targetPipe)->brokenPipeHandler = (ILibProcessPipe_GenericBrokenPipeHandler)handler;
}

ILibProcessPipe_PipeObject* ILibProcessPipe_CreatePipe(ILibProcessPipe_Manager manager, int pipeBufferSize, ILibProcessPipe_GenericBrokenPipeHandler brokenPipeHandler, int extraMemorySize)
{
	ILibProcessPipe_PipeObject* retVal = NULL;
#ifdef WIN32
	unsigned int pipeCounter = 0;
	char pipeName[255];
	SECURITY_ATTRIBUTES saAttr;
#else
	int fd[2];
#endif

	retVal = (ILibProcessPipe_PipeObject*)ILibMemory_SmartAllocateEx(sizeof(ILibProcessPipe_PipeObject), extraMemorySize);
	retVal->brokenPipeHandler = brokenPipeHandler;
	retVal->manager = (ILibProcessPipe_Manager_Object*)manager;

#ifdef WIN32
	saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
	saAttr.bInheritHandle = TRUE;
	saAttr.lpSecurityDescriptor = NULL;

	do
	{
		sprintf_s(pipeName, sizeof(pipeName), "\\\\.\\pipe\\%p%u", (void*)retVal, pipeCounter++);
		retVal->mPipe_ReadEnd = CreateNamedPipeA(pipeName, FILE_FLAG_FIRST_PIPE_INSTANCE | PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED, PIPE_TYPE_BYTE, 1, pipeBufferSize, pipeBufferSize, 0, &saAttr);
		if (retVal->mPipe_ReadEnd == (HANDLE)INVALID_HANDLE_VALUE) { ILIBCRITICALEXIT(254); }
	} while (retVal->mPipe_ReadEnd == (HANDLE)ERROR_ACCESS_DENIED);

	if ((retVal->mOverlapped = (struct _OVERLAPPED*)malloc(sizeof(struct _OVERLAPPED))) == NULL) { ILIBCRITICALEXIT(254); }
	memset(retVal->mOverlapped, 0, sizeof(struct _OVERLAPPED));
	if ((retVal->mOverlapped->hEvent = CreateEvent(NULL, TRUE, FALSE, NULL)) == NULL) { ILIBCRITICALEXIT(254); }

	retVal->mPipe_WriteEnd = CreateFileA(pipeName, GENERIC_WRITE, 0, &saAttr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (retVal->mPipe_WriteEnd == INVALID_HANDLE_VALUE) { ILIBCRITICALEXIT(254); }
#else
	if(pipe(fd)==0) 
	{
		fcntl(fd[0], F_SETFL, O_NONBLOCK); 
		fcntl(fd[1], F_SETFL, O_NONBLOCK);
		retVal->mPipe_ReadEnd = fd[0];
		retVal->mPipe_WriteEnd = fd[1];
	}
#endif
	
	return retVal;
}

#ifdef WIN32

typedef struct ILibProcessPipe_Process_Destroy_WinRunThread_Data
{
	ILibProcessPipe_Process_Object *pj;
	HANDLE h;
}ILibProcessPipe_Process_Destroy_WinRunThread_Data;
void __stdcall ILibProcessPipe_Process_Destroy_WinRunThread(ULONG_PTR obj)
{
	ILibProcessPipe_Process_Destroy_WinRunThread_Data *data = (ILibProcessPipe_Process_Destroy_WinRunThread_Data*)obj;
	if (ILibMemory_CanaryOK(data) && ILibMemory_CanaryOK(data->pj))
	{
		if (data->pj->exiting == 0)
		{
			if (ILibMemory_CanaryOK(data) && ILibMemory_CanaryOK(data->pj) && data->pj->stdIn != NULL) { ILibProcessPipe_FreePipe(data->pj->stdIn); }
			if (ILibMemory_CanaryOK(data) && ILibMemory_CanaryOK(data->pj) && data->pj->stdOut != NULL) { ILibProcessPipe_FreePipe(data->pj->stdOut); }
			if (ILibMemory_CanaryOK(data) && ILibMemory_CanaryOK(data->pj) && data->pj->stdErr != NULL) { ILibProcessPipe_FreePipe(data->pj->stdErr); }
			if (ILibMemory_CanaryOK(data) && ILibMemory_CanaryOK(data->pj)) { ILibMemory_Free(data->pj); }
		}
	}
	SetEvent(data->h);
}
#endif
void ILibProcessPipe_Process_Destroy(ILibProcessPipe_Process_Object *p)
{
	if (!ILibMemory_CanaryOK(p)) { return; }

#ifdef WIN32
	//ToDo: Do Something here

	//ILibProcessPipe_Process_Destroy_WinRunThread_Data *data = ILibMemory_AllocateA(sizeof(ILibProcessPipe_Process_Destroy_WinRunThread_Data));
	//data->pj = p;
	//data->h = CreateEvent(NULL, TRUE, FALSE, NULL);
	//// We can't destroy this now, because we're on the MicrostackThread. We must destroy this on the WindowsRunLoop Thread.
	//QueueUserAPC((PAPCFUNC)ILibProcessPipe_Process_Destroy_WinRunThread, p->parent->workerThread, (ULONG_PTR)data);
	//WaitForSingleObjectEx(data->h, 3000, TRUE);
	//CloseHandle(data->h);
#else
	if (p->exiting != 0) { return; }
	if (p->stdIn != NULL) { ILibProcessPipe_FreePipe(p->stdIn); }
	if (p->stdOut != NULL) { ILibProcessPipe_FreePipe(p->stdOut); }
	if (p->stdErr != NULL) { ILibProcessPipe_FreePipe(p->stdErr); }
	ILibMemory_Free(p);
#endif
}
#ifndef WIN32
void ILibProcessPipe_Process_BrokenPipeSink_DestroyHandler(void *object)
{
	ILibProcessPipe_Process_Destroy((ILibProcessPipe_Process_Object*)object);
}
void ILibProcessPipe_Process_BrokenPipeSink(ILibProcessPipe_Pipe sender)
{
	ILibProcessPipe_Process_Object *p = ((ILibProcessPipe_PipeObject*)sender)->mProcess;
	int status;
	if (ILibIsRunningOnChainThread(((ILibProcessPipe_PipeObject*)sender)->manager->ChainLink.ParentChain) != 0)
	{
		// This was called from the Reader
		if (p->exitHandler != NULL)
		{

			waitpid((pid_t)p->PID, &status, 0);
			p->exitHandler(p, WEXITSTATUS(status), p->userObject);
		}

		// Unwind the stack, and destroy the process object
		ILibLifeTime_Add(ILibGetBaseTimer(p->parent->ChainLink.ParentChain), p, 0, ILibProcessPipe_Process_BrokenPipeSink_DestroyHandler, NULL);
	}
}
#endif

void ILibProcessPipe_Process_SoftKill(ILibProcessPipe_Process p)
{
	ILibProcessPipe_Process_Object* j = (ILibProcessPipe_Process_Object*)p;
	if (!ILibMemory_CanaryOK(p)) { return; }

#ifdef WIN32
	TerminateProcess(j->hProcess, 1067);
#else
	int code;
	kill((pid_t)j->PID, SIGKILL);
	waitpid((pid_t)j->PID, &code, 0);
#endif
}

ILibProcessPipe_Process ILibProcessPipe_Manager_SpawnProcessEx4(ILibProcessPipe_Manager pipeManager, char* target, char* const* parameters, ILibProcessPipe_SpawnTypes spawnType, void *sid, void *envvars, int extraMemorySize)
{
	ILibProcessPipe_Process_Object* retVal = NULL;
	int needSetSid = ((spawnType & ILibProcessPipe_SpawnTypes_POSIX_DETACHED) == ILibProcessPipe_SpawnTypes_POSIX_DETACHED);
	if (needSetSid != 0) { spawnType ^= ILibProcessPipe_SpawnTypes_POSIX_DETACHED; }

#ifdef WIN32
	STARTUPINFOA info = { 0 };
	PROCESS_INFORMATION processInfo = { 0 };
	SECURITY_ATTRIBUTES saAttr;
	char* parms = NULL;
	DWORD sessionId;
	HANDLE token = NULL, userToken = NULL, procHandle = NULL;
	int allocParms = 0;
	
	ZeroMemory(&processInfo, sizeof(PROCESS_INFORMATION));
	ZeroMemory(&info, sizeof(STARTUPINFOA));


	if (spawnType != ILibProcessPipe_SpawnTypes_SPECIFIED_USER && spawnType != ILibProcessPipe_SpawnTypes_DEFAULT && (sessionId = WTSGetActiveConsoleSessionId()) == 0xFFFFFFFF) { return(NULL); } // No session attached to console, but requested to execute as logged in user
	if (spawnType != ILibProcessPipe_SpawnTypes_DEFAULT)
	{
		procHandle = GetCurrentProcess();
		if (OpenProcessToken(procHandle, TOKEN_DUPLICATE, &token) == 0) { ILIBMARKPOSITION(2); return(NULL); }
		if (DuplicateTokenEx(token, MAXIMUM_ALLOWED, 0, SecurityImpersonation, TokenPrimary, &userToken) == 0) { CloseHandle(token); ILIBMARKPOSITION(2); return(NULL); }
		if (spawnType == ILibProcessPipe_SpawnTypes_SPECIFIED_USER) { sessionId = (DWORD)(uint64_t)sid; }
		if (SetTokenInformation(userToken, (TOKEN_INFORMATION_CLASS)TokenSessionId, &sessionId, sizeof(sessionId)) == 0) { CloseHandle(token); CloseHandle(userToken); ILIBMARKPOSITION(2); return(NULL); }
		if (spawnType == ILibProcessPipe_SpawnTypes_WINLOGON) { info.lpDesktop = "Winsta0\\Winlogon"; }
	}
	if (parameters != NULL && parameters[0] != NULL && parameters[1] == NULL)
	{
		parms = parameters[0];
	}
	else if (parameters != NULL && parameters[0] != NULL && parameters[1] != NULL)
	{
		int len = 0;
		int i = 0;
		int sz = 0;

		while (parameters[i] != NULL)
		{
			sz += ((int)strnlen_s(parameters[i++], sizeof(ILibScratchPad2)) + 1);
		}
		sz += (i - 1); // Need to make room for delimiter characters
		parms = (char*)malloc(sz);
		i = 0; len = 0;
		allocParms = 1;

		while (parameters[i] != NULL)
		{
			len += sprintf_s(parms + len, sz - len, "%s%s", (i == 0) ? "" : " ", parameters[i]);
			++i;
		}
	}

	saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
	saAttr.bInheritHandle = TRUE;
	saAttr.lpSecurityDescriptor = NULL;
#else
	pid_t pid;
#endif

	retVal = (ILibProcessPipe_Process_Object*)ILibMemory_SmartAllocate(sizeof(ILibProcessPipe_Process_Object));
	if (spawnType != ILibProcessPipe_SpawnTypes_DETACHED)
	{
		retVal->stdErr = ILibProcessPipe_CreatePipe(pipeManager, 4096, NULL, extraMemorySize);
		retVal->stdErr->mProcess = retVal;
	}
	retVal->parent = (ILibProcessPipe_Manager_Object*)pipeManager;
	retVal->chain = retVal->parent->ChainLink.ParentChain;
#ifdef WIN32

	info.cb = sizeof(STARTUPINFOA);
	if (spawnType != ILibProcessPipe_SpawnTypes_DETACHED)
	{
		retVal->stdIn = ILibProcessPipe_CreatePipe(pipeManager, 4096, NULL, extraMemorySize);
		retVal->stdIn->mProcess = retVal;
		retVal->stdOut = ILibProcessPipe_CreatePipe(pipeManager, 4096, NULL, extraMemorySize);
		retVal->stdOut->mProcess = retVal;

		ILibProcessPipe_PipeObject_DisableInherit(&(retVal->stdIn->mPipe_WriteEnd));
		ILibProcessPipe_PipeObject_DisableInherit(&(retVal->stdOut->mPipe_ReadEnd));
		ILibProcessPipe_PipeObject_DisableInherit(&(retVal->stdErr->mPipe_ReadEnd));

		info.hStdError = retVal->stdErr->mPipe_WriteEnd;
		info.hStdInput = retVal->stdIn->mPipe_ReadEnd;
		info.hStdOutput = retVal->stdOut->mPipe_WriteEnd;
		info.dwFlags |= STARTF_USESTDHANDLES;
	}

	if (((spawnType == ILibProcessPipe_SpawnTypes_DEFAULT || spawnType == ILibProcessPipe_SpawnTypes_DETACHED) && !CreateProcessA(target, parms, NULL, NULL, spawnType == ILibProcessPipe_SpawnTypes_DETACHED ? FALSE: TRUE, CREATE_NO_WINDOW | (needSetSid !=0? (DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP) : 0x00), envvars, NULL, &info, &processInfo)) ||
		(spawnType != ILibProcessPipe_SpawnTypes_DEFAULT && !CreateProcessAsUserA(userToken, target, parms, NULL, NULL, TRUE, CREATE_NO_WINDOW | (needSetSid != 0 ? (DETACHED_PROCESS| CREATE_NEW_PROCESS_GROUP) : 0x00), envvars, NULL, &info, &processInfo)))
	{
		if (spawnType != ILibProcessPipe_SpawnTypes_DETACHED)
		{
			ILibProcessPipe_FreePipe(retVal->stdErr);
			ILibProcessPipe_FreePipe(retVal->stdOut);
			ILibProcessPipe_FreePipe(retVal->stdIn);
		}
		if (allocParms != 0) { free(parms); }
		ILibMemory_Free(retVal);
		if (token != NULL) { CloseHandle(token); }
		if (userToken != NULL) { CloseHandle(userToken); }
		return(NULL);
	}


	if (allocParms != 0) { free(parms); }
	if (spawnType != ILibProcessPipe_SpawnTypes_DETACHED)
	{
		CloseHandle(retVal->stdOut->mPipe_WriteEnd);	retVal->stdOut->mPipe_WriteEnd = NULL;
		CloseHandle(retVal->stdErr->mPipe_WriteEnd);	retVal->stdErr->mPipe_WriteEnd = NULL;
		CloseHandle(retVal->stdIn->mPipe_ReadEnd);		retVal->stdIn->mPipe_ReadEnd = NULL;
	}

	retVal->hProcess = processInfo.hProcess;
	if (processInfo.hThread != NULL) CloseHandle(processInfo.hThread);
	retVal->PID = processInfo.dwProcessId;

	if (token != NULL) { CloseHandle(token); token = NULL; }
	if (userToken != NULL) { CloseHandle(userToken); userToken = NULL; }
#else
	int UID = (int)(uint64_t)(ILibPtrCAST)sid;
#ifdef __APPLE__
	if (spawnType == ILibProcessPipe_SpawnTypes_TERM) { spawnType = ILibProcessPipe_SpawnTypes_DEFAULT; }
#endif
	if (spawnType == ILibProcessPipe_SpawnTypes_TERM)
	{
#ifndef __APPLE__
		int pipe;
		struct winsize w;
		w.ws_row = CONSOLE_SCREEN_HEIGHT;
		w.ws_col = CONSOLE_SCREEN_WIDTH;
		w.ws_xpixel = 0;
		w.ws_ypixel = 0;
		pid = forkpty(&pipe, NULL, NULL, &w);
		retVal->stdIn = ILibProcessPipe_Pipe_CreateFromExistingWithExtraMemory(pipeManager, pipe, extraMemorySize);
		retVal->stdIn->mProcess = retVal;
		retVal->stdOut = ILibProcessPipe_Pipe_CreateFromExistingWithExtraMemory(pipeManager, pipe, extraMemorySize);
		ILibProcessPipe_Pipe_SetBrokenPipeHandler(retVal->stdOut, ILibProcessPipe_Process_BrokenPipeSink);
		retVal->stdOut->mProcess = retVal;
#else
		pid = 0; // Apple LLVM is being dumb, and throws a warning if I don't do this, even tho it'll never run
#endif
	}
	else
	{
		if (spawnType != ILibProcessPipe_SpawnTypes_DETACHED)
		{
			retVal->stdIn = ILibProcessPipe_CreatePipe(pipeManager, 4096, NULL, extraMemorySize);
			retVal->stdIn->mProcess = retVal;
			retVal->stdOut = ILibProcessPipe_CreatePipe(pipeManager, 4096, (ILibProcessPipe_GenericBrokenPipeHandler)ILibProcessPipe_Process_BrokenPipeSink, extraMemorySize);
			retVal->stdOut->mProcess = retVal;
		}
#ifdef __APPLE__
		if (needSetSid == 0)
		{
			pid = vfork();
		}
		else
		{
			pid = fork();
		}
#else
		pid = vfork();
#endif
	}
	if (pid < 0)
	{
		if (spawnType != ILibProcessPipe_SpawnTypes_DETACHED)
		{
			ILibProcessPipe_FreePipe(retVal->stdErr);
			ILibProcessPipe_FreePipe(retVal->stdOut);
			ILibProcessPipe_FreePipe(retVal->stdIn);
		}
		ILibMemory_Free(retVal);
		return(NULL);
	}
	if (pid == 0)
	{
		if (spawnType != ILibProcessPipe_SpawnTypes_DETACHED)
		{
			close(retVal->stdErr->mPipe_ReadEnd); //close read end of stderr pipe
			dup2(retVal->stdErr->mPipe_WriteEnd, STDERR_FILENO);
			close(retVal->stdErr->mPipe_WriteEnd);
		}
		if (spawnType == ILibProcessPipe_SpawnTypes_TERM)
		{
			putenv("TERM=xterm");
		}
		else
		{
			if (spawnType != ILibProcessPipe_SpawnTypes_DETACHED)
			{
				close(retVal->stdIn->mPipe_WriteEnd); //close write end of stdin pipe
				close(retVal->stdOut->mPipe_ReadEnd); //close read end of stdout pipe

				dup2(retVal->stdIn->mPipe_ReadEnd, STDIN_FILENO);
				dup2(retVal->stdOut->mPipe_WriteEnd, STDOUT_FILENO);

				close(retVal->stdIn->mPipe_ReadEnd);
				close(retVal->stdOut->mPipe_WriteEnd);

				int f = fcntl(STDIN_FILENO, F_GETFL);
				f &= ~O_NONBLOCK;
				fcntl(STDIN_FILENO, F_SETFL, f);

				f = fcntl(STDOUT_FILENO, F_GETFL);
				f &= ~O_NONBLOCK;
				fcntl(STDOUT_FILENO, F_SETFL, f);
			}
		}
		if (UID != -1 && UID != 0)
		{
			ignore_result(setuid((uid_t)UID));
		}
		if (needSetSid != 0)
		{
			ignore_result(setsid());
		}
		while (envvars != NULL && ((char**)envvars)[0] != NULL)
		{
			setenv(((char**)envvars)[0], ((char**)envvars)[1], 1);
			envvars = (void*)((char*)envvars + 2 * sizeof(char*));
		}
		execv(target, parameters);
		_exit(1);
	}

	if (spawnType != ILibProcessPipe_SpawnTypes_TERM && spawnType != ILibProcessPipe_SpawnTypes_DETACHED)
	{
		close(retVal->stdIn->mPipe_ReadEnd); retVal->stdIn->mPipe_ReadEnd = -1;
		close(retVal->stdOut->mPipe_WriteEnd); retVal->stdOut->mPipe_WriteEnd = -1;
	}
	if (spawnType != ILibProcessPipe_SpawnTypes_DETACHED)
	{
		close(retVal->stdErr->mPipe_WriteEnd); retVal->stdErr->mPipe_WriteEnd = -1;
	}
	retVal->PID = pid;
#endif
	return retVal;
}
int ILibProcessPipe_Process_IsDetached(ILibProcessPipe_Process p)
{
	return(((ILibProcessPipe_Process_Object*)p)->stdErr == NULL && ((ILibProcessPipe_Process_Object*)p)->stdIn == NULL && ((ILibProcessPipe_Process_Object*)p)->stdOut == NULL);
}
void ILibProcessPipe_Pipe_SwapBuffers(ILibProcessPipe_Pipe obj, char* newBuffer, int newBufferLen, int newBufferReadOffset, int newBufferTotalBytesRead, char **oldBuffer, int *oldBufferLen, int *oldBufferReadOffset, int *oldBufferTotalBytesRead)
{
	ILibProcessPipe_PipeObject *pipeObject = (ILibProcessPipe_PipeObject*)obj;

	*oldBuffer = pipeObject->buffer;
	if (oldBufferLen != NULL) { *oldBufferLen = pipeObject->bufferSize; }
	if (oldBufferReadOffset != NULL) { *oldBufferReadOffset = pipeObject->readOffset; }
	if (oldBufferTotalBytesRead != NULL) { *oldBufferTotalBytesRead = pipeObject->totalRead; }

	pipeObject->buffer = newBuffer;
	pipeObject->bufferSize = newBufferLen;
	pipeObject->readOffset = newBufferReadOffset;
	pipeObject->totalRead = newBufferTotalBytesRead;
}

#ifdef WIN32
BOOL ILibProcessPipe_Process_ReadHandler(HANDLE event, ILibWaitHandle_ErrorStatus errors, void* user)
#else
void ILibProcessPipe_Process_ReadHandler(void* user)
#endif
{
#ifdef WIN32
	if (errors != ILibWaitHandle_ErrorStatus_NONE) { return(FALSE); }
#endif
	ILibProcessPipe_PipeObject *pipeObject = (ILibProcessPipe_PipeObject*)user;
	int consumed;
	int err=0;
	
#ifdef WIN32
	BOOL result;
	DWORD bytesRead;
	UNREFERENCED_PARAMETER(event);
#else
	int bytesRead;
#endif
	pipeObject->processingLoop = 1;
	do
	{
#ifdef WIN32
		err = 0;
		result = GetOverlappedResult(pipeObject->mPipe_ReadEnd, pipeObject->mOverlapped, &bytesRead, FALSE);
		//printf("Overlapped(%p): %d bytes\n", pipeObject->mPipe_ReadEnd, bytesRead);
		if (result == FALSE || bytesRead == 0)
		{
			err = GetLastError();
			break;
		}
#else
		bytesRead = (int)read(pipeObject->mPipe_ReadEnd, pipeObject->buffer + pipeObject->readOffset + pipeObject->totalRead, pipeObject->bufferSize - pipeObject->totalRead);
		if (bytesRead <= 0)
		{
			break;
		}

#endif
		pipeObject->totalRead += bytesRead;
		ILibRemoteLogging_printf(ILibChainGetLogger(pipeObject->manager->ChainLink.ParentChain), ILibRemoteLogging_Modules_Microstack_Pipe, ILibRemoteLogging_Flags_VerbosityLevel_5, "ILibProcessPipe[ReadHandler]: %u bytes read on Pipe: %p", bytesRead, (void*)pipeObject);

		if (pipeObject->handler == NULL)
		{
			//
			// Since the user doesn't care about the data, we'll just empty the buffer
			//
			pipeObject->readOffset = 0;
			pipeObject->totalRead = 0;
			continue;
		}

		while (pipeObject->PAUSED == 0)
		{
			consumed = 0;
			ILibRemoteLogging_printf(ILibChainGetLogger(pipeObject->manager->ChainLink.ParentChain), ILibRemoteLogging_Modules_Microstack_Generic, ILibRemoteLogging_Flags_VerbosityLevel_5, "ProcessPipe: buffer/%p offset/%d totalRead/%d", (void*)pipeObject->buffer, pipeObject->readOffset, pipeObject->totalRead);
			((ILibProcessPipe_GenericReadHandler)pipeObject->handler)(pipeObject->buffer + pipeObject->readOffset, pipeObject->totalRead, &consumed, pipeObject->user1, pipeObject->user2);
			if (consumed == 0)
			{
				//
				// None of the buffer was consumed
				//
				ILibRemoteLogging_printf(ILibChainGetLogger(pipeObject->manager->ChainLink.ParentChain), ILibRemoteLogging_Modules_Microstack_Pipe, ILibRemoteLogging_Flags_VerbosityLevel_5, "ILibProcessPipe[ReadHandler]: No bytes consumed on Pipe: %p", (void*)pipeObject);

				//
				// We need to move the memory to the start of the buffer, or else we risk running past the end, if we keep reading like this
				//
				memmove_s(pipeObject->buffer, pipeObject->bufferSize, pipeObject->buffer + pipeObject->readOffset, pipeObject->totalRead);
				pipeObject->readOffset = 0;

				break; // Break out of inner while loop
			}
			else if (consumed == pipeObject->totalRead)
			{
				//
				// Entire Buffer was consumed
				//
				pipeObject->readOffset = 0;
				pipeObject->totalRead = 0;

				ILibRemoteLogging_printf(ILibChainGetLogger(pipeObject->manager->ChainLink.ParentChain), ILibRemoteLogging_Modules_Microstack_Pipe, ILibRemoteLogging_Flags_VerbosityLevel_5, "ILibProcessPipe[ReadHandler]: ReadBuffer drained on Pipe: %p", (void*)pipeObject);
				break; // Break out of inner while loop
			}
			else
			{
				//
				// Only part of the buffer was consumed
				//
				pipeObject->readOffset += consumed;
				pipeObject->totalRead -= consumed;
			}
		}

		if (pipeObject->bufferSize - pipeObject->totalRead == 0)
		{
			pipeObject->buffer = (char*)realloc(pipeObject->buffer, pipeObject->bufferSize * 2);
			if (pipeObject->buffer == NULL) { ILIBCRITICALEXIT(254); }
			pipeObject->bufferSize = pipeObject->bufferSize * 2;
		}
#ifdef WIN32
		if (pipeObject->PAUSED == 0)
		{
			if (ReadFile(pipeObject->mPipe_ReadEnd, pipeObject->buffer + pipeObject->readOffset + pipeObject->totalRead, pipeObject->bufferSize - pipeObject->totalRead, NULL, pipeObject->mOverlapped) != TRUE)
			{
				break;
			}
		}
#endif
	}
#ifdef WIN32
	while (pipeObject->PAUSED == 0); // Note: This is actually the end of a do-while loop
	if(bytesRead == 0 || (err != ERROR_IO_PENDING && err != 0 && pipeObject->PAUSED == 0))
#else
	while(pipeObject->PAUSED == 0); // Note: This is actually the end of a do-while loop
	err = 0;
	if (bytesRead == 0 || ((err = errno) != EAGAIN && errno != EWOULDBLOCK && pipeObject->PAUSED == 0))
#endif
	{
		//printf("Broken Pipe(%p)? (err: %d, PAUSED: %d, totalRead: %d\n", pipeObject->mPipe_ReadEnd, err, pipeObject->PAUSED, pipeObject->totalRead);
		//
		// Broken Pipe
		//
		ILibRemoteLogging_printf(ILibChainGetLogger(pipeObject->manager->ChainLink.ParentChain), ILibRemoteLogging_Modules_Microstack_Pipe, ILibRemoteLogging_Flags_VerbosityLevel_1, "ILibProcessPipe[ReadHandler]: BrokenPipe(%d) on Pipe: %p", err, (void*)pipeObject);
#ifdef WIN32
		ILibProcessPipe_WaitHandle_Remove(pipeObject->manager, pipeObject->mOverlapped->hEvent); // Pipe Broken, so remove ourselves from the processing loop
		ILibLinkedList_Remove(ILibLinkedList_GetNode_Search(pipeObject->manager->ActivePipes, NULL, pipeObject));
#else
		void *pipenode = ILibLinkedList_GetNode_Search(pipeObject->manager->ActivePipes, NULL, pipeObject);
		if (pipenode != NULL)
		{
			// Flag this node for removal
			((int*)ILibLinkedList_GetExtendedMemory(pipenode))[0] = 1;
		}
#endif
		if (pipeObject->brokenPipeHandler != NULL) 
		{
			((ILibProcessPipe_GenericBrokenPipeHandler)pipeObject->brokenPipeHandler)(pipeObject); 
		}
#ifdef WIN32
		return(FALSE);
#else
		return;
#endif
	}
	else
	{
		ILibRemoteLogging_printf(ILibChainGetLogger(pipeObject->manager->ChainLink.ParentChain), ILibRemoteLogging_Modules_Microstack_Pipe, ILibRemoteLogging_Flags_VerbosityLevel_5, "ILibProcessPipe[ReadHandler]: Pipe: %p [EMPTY]", (void*)pipeObject);
	}
	pipeObject->processingLoop = 0;
	ILibRemoteLogging_printf(ILibChainGetLogger(pipeObject->manager->ChainLink.ParentChain), ILibRemoteLogging_Modules_Microstack_Generic, ILibRemoteLogging_Flags_VerbosityLevel_1, "ILibProcessPipe[ReadHandler]: Pipe: %p [EMPTY]", (void*)pipeObject);

#ifdef WIN32
	return(TRUE);
#else
	return;
#endif
}
#ifdef WIN32
BOOL ILibProcessPipe_Process_WindowsWriteHandler(HANDLE event, ILibWaitHandle_ErrorStatus errors, void* user)
{
	if (errors != ILibWaitHandle_ErrorStatus_NONE) { return(FALSE); }
	ILibProcessPipe_PipeObject *pipeObject = (ILibProcessPipe_PipeObject*)user;
	BOOL result;
	DWORD bytesWritten;
	ILibProcessPipe_WriteData* data;
	
	UNREFERENCED_PARAMETER(event);
	result = GetOverlappedResult(pipeObject->mPipe_WriteEnd, pipeObject->mOverlapped, &bytesWritten, FALSE);
	if (result == FALSE)
	{ 
		// Broken Pipe
		ILibProcessPipe_WaitHandle_Remove(pipeObject->manager, pipeObject->mOverlapped->hEvent); // Pipe Broken, so remove ourselves from the processing loop
		ILibRemoteLogging_printf(ILibChainGetLogger(pipeObject->manager->ChainLink.ParentChain), ILibRemoteLogging_Modules_Microstack_Pipe, ILibRemoteLogging_Flags_VerbosityLevel_1, "ILibProcessPipe[WriteHandler]: BrokenPipe(%d) on Pipe: %p", GetLastError(), (void*)pipeObject);
		if (pipeObject->brokenPipeHandler != NULL) { ((ILibProcessPipe_GenericBrokenPipeHandler)pipeObject->brokenPipeHandler)(pipeObject); }
		ILibProcessPipe_FreePipe(pipeObject);
		return(FALSE);
	}

	ILibQueue_Lock(pipeObject->WriteBuffer);
	while ((data = (ILibProcessPipe_WriteData*)ILibQueue_DeQueue(pipeObject->WriteBuffer)) != NULL)
	{
		ILibProcessPipe_WriteData_Destroy(data);
		data = (ILibProcessPipe_WriteData*)ILibQueue_PeekQueue(pipeObject->WriteBuffer);
		if (data != NULL)
		{
			result = WriteFile(pipeObject->mPipe_WriteEnd, data->buffer, data->bufferLen, NULL, pipeObject->mOverlapped);
			if (result == TRUE) { continue; }
			if (GetLastError() != ERROR_IO_PENDING)
			{
				// Broken Pipe
				ILibQueue_UnLock(pipeObject->WriteBuffer);
				ILibRemoteLogging_printf(ILibChainGetLogger(pipeObject->manager->ChainLink.ParentChain), ILibRemoteLogging_Modules_Microstack_Pipe, ILibRemoteLogging_Flags_VerbosityLevel_1, "ILibProcessPipe[WriteHandler]: BrokenPipe(%d) on Pipe: %p", GetLastError(), (void*)pipeObject);
				if (pipeObject->brokenPipeHandler != NULL) { ((ILibProcessPipe_GenericBrokenPipeHandler)pipeObject->brokenPipeHandler)(pipeObject); }
				ILibProcessPipe_FreePipe(pipeObject);
				return(FALSE);
			}
			break;
		}
	}
	if (ILibQueue_IsEmpty(pipeObject->WriteBuffer) != 0)
	{
		ILibProcessPipe_WaitHandle_Remove(pipeObject->manager, pipeObject->mOverlapped->hEvent);
		ILibQueue_UnLock(pipeObject->WriteBuffer);
		if (pipeObject->handler != NULL) ((ILibProcessPipe_GenericSendOKHandler)pipeObject->handler)(pipeObject->user1, pipeObject->user2);
	}
	else
	{
		ILibQueue_UnLock(pipeObject->WriteBuffer);
	}
	return(TRUE);
}
#endif
void ILibProcessPipe_Process_SetWriteHandler(ILibProcessPipe_PipeObject *pipeObject, ILibProcessPipe_GenericSendOKHandler handler, void* user1, void* user2)
{
	pipeObject->handler = (void*)handler;
	pipeObject->user1 = user1;
	pipeObject->user2 = user2;
}

void ILibProcessPipe_Process_StartPipeReaderWriterEx(void *object)
{
	ILibProcessPipe_PipeObject* pipeObject = (ILibProcessPipe_PipeObject*)object;
	ILibLinkedList_AddTail(pipeObject->manager->ActivePipes, pipeObject);
}

void ILibProcessPipe_Pipe_Pause(ILibProcessPipe_Pipe pipeObject)
{
	ILibProcessPipe_PipeObject *p = (ILibProcessPipe_PipeObject*)pipeObject;
	p->PAUSED = 1;

#ifdef WIN32
	if (p->mOverlapped == NULL)
	{
		// Overlapped isn't supported, so using a separate reader thread
		ResetEvent(p->mPipe_Reader_ResumeEvent);
	}
	else
	{
		ILibRemoteLogging_printf(ILibChainGetLogger(p->manager->ChainLink.ParentChain), ILibRemoteLogging_Modules_Microstack_Generic, ILibRemoteLogging_Flags_VerbosityLevel_1, "ProcessPipe.Pause()");
		ILibProcessPipe_WaitHandle_Remove(p->manager, p->mOverlapped->hEvent);
	}
#else
	ILibLinkedList_Remove(ILibLinkedList_GetNode_Search(p->manager->ActivePipes, NULL, pipeObject));
#endif
}


void ILibProcessPipe_Pipe_ResumeEx_ContinueProcessing(ILibProcessPipe_PipeObject *p)
{
	int consumed;
	p->PAUSED = 0;
	p->processingLoop = 1;
	while (p->PAUSED == 0 && p->totalRead > 0)
	{
		consumed = 0;
		((ILibProcessPipe_GenericReadHandler)p->handler)(p->buffer + p->readOffset, p->totalRead, &consumed, p->user1, p->user2);
		if (consumed == 0)
		{
			//
			// None of the buffer was consumed
			//
			ILibRemoteLogging_printf(ILibChainGetLogger(p->manager->ChainLink.ParentChain), ILibRemoteLogging_Modules_Microstack_Pipe, ILibRemoteLogging_Flags_VerbosityLevel_5, "ILibProcessPipe[ReadHandler]: No bytes consumed on Pipe: %p", (void*)p);

			//
			// We need to move the memory to the start of the buffer, or else we risk running past the end, if we keep reading like this
			//
			memmove_s(p->buffer, p->bufferSize, p->buffer + p->readOffset, p->totalRead);
			p->readOffset = 0;
			break;
		}
		else if (consumed == p->totalRead)
		{
			//
			// Entire Buffer was consumed
			//
			p->readOffset = 0;
			p->totalRead = 0;

			ILibRemoteLogging_printf(ILibChainGetLogger(p->manager->ChainLink.ParentChain), ILibRemoteLogging_Modules_Microstack_Pipe, ILibRemoteLogging_Flags_VerbosityLevel_5, "ILibProcessPipe[ReadHandler]: ReadBuffer drained on Pipe: %p", (void*)p);
			break; // Break out of inner while loop
		}
		else
		{
			//
			// Only part of the buffer was consumed
			//
			p->readOffset += consumed;
			p->totalRead -= consumed;
		}
	}
	p->processingLoop = 0;
}

void ILibProcessPipe_Pipe_ResumeEx(ILibProcessPipe_PipeObject* p)
{
	if (!ILibMemory_CanaryOK(p)) { return; }
	ILibRemoteLogging_printf(ILibChainGetLogger(p->manager->ChainLink.ParentChain), ILibRemoteLogging_Modules_Microstack_Generic, ILibRemoteLogging_Flags_VerbosityLevel_1, "ProcessPipe.ResumeEx(): processingLoop = %d", p->processingLoop);

#ifdef WIN32
	if (!ILibIsRunningOnChainThread(p->manager->ChainLink.ParentChain))
	{
		QueueUserAPC((PAPCFUNC)NULL, ILibChain_GetMicrostackThreadHandle(p->manager->ChainLink.ParentChain), (ULONG_PTR)p);
		return;
	}
	ILibProcessPipe_WaitHandle_Add(p->manager, p->mOverlapped->hEvent, p, ILibProcessPipe_Process_ReadHandler);
	p->PAUSED = 0;
	return;
#endif

	ILibProcessPipe_Pipe_ResumeEx_ContinueProcessing(p);
	if (p->PAUSED == 0)
	{
		ILibLifeTime_Add(ILibGetBaseTimer(p->manager->ChainLink.ParentChain), p, 0, &ILibProcessPipe_Process_StartPipeReaderWriterEx, NULL); // Need to context switch to Chain Thread
	}
}
void ILibProcessPipe_Pipe_Resume(ILibProcessPipe_Pipe pipeObject)
{
	ILibProcessPipe_PipeObject *p = (ILibProcessPipe_PipeObject*)pipeObject;
	if (!ILibMemory_CanaryOK(p)) { return; }
#ifdef WIN32
	if (p->mOverlapped == NULL)
	{
		SetEvent(p->mPipe_Reader_ResumeEvent);
	}
	else
	{
		ILibProcessPipe_Pipe_ResumeEx(p);
		if (p->mProcess != NULL && p->mProcess->hProcess_needAdd != 0)
		{
			p->mProcess->hProcess_needAdd = 0;
			ILibProcessPipe_WaitHandle_Add(p->manager, p->mProcess->hProcess, p->mProcess, ILibProcessPipe_Process_OnExit);
		}
	}
#else
	ILibProcessPipe_Pipe_ResumeEx(p);
#endif
}

#ifdef WIN32
DWORD ILibProcessPipe_Pipe_BackgroundReader(void *arg)
{
	ILibProcessPipe_PipeObject *pipeObject = (ILibProcessPipe_PipeObject*)arg;
	DWORD bytesRead = 0;
	int consumed = 0;

	while (pipeObject->PAUSED == 0 || WaitForSingleObject(pipeObject->mPipe_Reader_ResumeEvent, INFINITE) == WAIT_OBJECT_0)
	{
		// Pipe is in ACTIVE state
		pipeObject->PAUSED = 0;

		while(consumed != 0 && pipeObject->PAUSED == 0 && (pipeObject->totalRead - pipeObject->readOffset)>0)
		{
			((ILibProcessPipe_GenericReadHandler)pipeObject->handler)(pipeObject->buffer + pipeObject->readOffset, pipeObject->totalRead - pipeObject->readOffset, &consumed, pipeObject->user1, pipeObject->user2);
			if (consumed == 0)
			{
				memmove_s(pipeObject->buffer, pipeObject->bufferSize, pipeObject->buffer + pipeObject->readOffset, pipeObject->totalRead - pipeObject->readOffset);
				pipeObject->readNewOffset = pipeObject->totalRead - pipeObject->readOffset;
				pipeObject->totalRead -= pipeObject->readOffset;
				pipeObject->readOffset = 0;
			}
			else if (consumed == (pipeObject->totalRead - pipeObject->readOffset))
			{
				// Entire buffer consumed
				pipeObject->readOffset = 0;
				pipeObject->totalRead = 0;
				pipeObject->readNewOffset = 0;
				consumed = 0;
			}
			else
			{
				// Partial Consumed
				pipeObject->readOffset += consumed;
			}
		}

		if (pipeObject->PAUSED == 1) { continue; }
		if (!ReadFile(pipeObject->mPipe_ReadEnd, pipeObject->buffer + pipeObject->readOffset + pipeObject->readNewOffset, pipeObject->bufferSize - pipeObject->readOffset - pipeObject->readNewOffset, &bytesRead, NULL)) { break; }

		consumed = 0;
		pipeObject->totalRead += bytesRead;
		((ILibProcessPipe_GenericReadHandler)pipeObject->handler)(pipeObject->buffer + pipeObject->readOffset, pipeObject->totalRead - pipeObject->readOffset, &consumed, pipeObject->user1, pipeObject->user2);
		pipeObject->readOffset += consumed;
		if (consumed == 0) 
		{ 
			pipeObject->readNewOffset = pipeObject->totalRead - pipeObject->readOffset;
		}
	}

	if (pipeObject->brokenPipeHandler != NULL) { pipeObject->brokenPipeHandler(pipeObject); }
	ILibProcessPipe_FreePipe(pipeObject);

	return 0;
}
#endif

void ILibProcessPipe_Process_StartPipeReader(ILibProcessPipe_PipeObject *pipeObject, int bufferSize, ILibProcessPipe_GenericReadHandler handler, void* user1, void* user2)
{
#ifdef WIN32
	BOOL result;
#endif

	if ((pipeObject->buffer = (char*)malloc(bufferSize)) == NULL) { ILIBCRITICALEXIT(254); }
	pipeObject->bufferSize = bufferSize;
	pipeObject->handler = (void*)handler;
	pipeObject->user1 = user1;
	pipeObject->user2 = user2;

#ifdef WIN32
	if (pipeObject->mOverlapped != NULL)
	{
		// This PIPE supports Overlapped I/O
		//printf("ReadFile(%p, %d, %d) (StartPipeReader)\n", pipeObject->mPipe_ReadEnd, 0, pipeObject->bufferSize);
		result = ReadFile(pipeObject->mPipe_ReadEnd, pipeObject->buffer, pipeObject->bufferSize, NULL, pipeObject->mOverlapped);
		ILibProcessPipe_WaitHandle_Add(pipeObject->manager, pipeObject->mOverlapped->hEvent, pipeObject, &ILibProcessPipe_Process_ReadHandler);
	}
	else
	{
		// This PIPE does NOT support overlapped I/O, so we have to fake it with a background thread
		pipeObject->mPipe_Reader_ResumeEvent = CreateEvent(NULL, TRUE, TRUE, NULL);
		ILibSpawnNormalThread(&ILibProcessPipe_Pipe_BackgroundReader, pipeObject);
	}
#else
	ILibLifeTime_Add(ILibGetBaseTimer(pipeObject->manager->ChainLink.ParentChain), pipeObject, 0, &ILibProcessPipe_Process_StartPipeReaderWriterEx, NULL); // Need to context switch to Chain Thread
#endif
}
void ILibProcessPipe_Process_PipeHandler_StdOut(char *buffer, int bufferLen, int* bytesConsumed, void* user1, void *user2)
{
	ILibProcessPipe_Process_Object *j = (ILibProcessPipe_Process_Object*)user1;
	if (user2 != NULL)
	{
		((ILibProcessPipe_Process_OutputHandler)user2)(j, buffer, bufferLen, bytesConsumed, j->userObject);
	}
}
void ILibProcessPipe_Process_PipeHandler_StdIn(void *user1, void *user2)
{
	ILibProcessPipe_Process_Object* j = (ILibProcessPipe_Process_Object*)user1;
	ILibProcessPipe_Process_SendOKHandler sendOk = (ILibProcessPipe_Process_SendOKHandler)user2;

	if (sendOk != NULL) sendOk(j, j->userObject);
}

#ifdef WIN32
void __stdcall ILibProcessPipe_Process_OnExit_ChainSink_DestroySink(ULONG_PTR obj)
{
	ILibProcessPipe_Process_Object* j = (ILibProcessPipe_Process_Object*)obj;
	if (j->exiting == 0) { ILibProcessPipe_Process_Destroy(j); }
}
void ILibProcessPipe_Process_OnExit_ChainSink(void *chain, void *user)
{
	ILibProcessPipe_Process_Object* j = (ILibProcessPipe_Process_Object*)user;
	DWORD exitCode;
	BOOL result;

	if (ILibChain_SelectInterrupted(j->chain) != 0)
	{
		// Winsock appears to be at a select call, so this must've been triggered by an APC, so we must unroll the callstack to continue,
		// because winsock is not re-entrant, so we cannot risk making another winsock call directly. 
		//
		ILibChain_RunOnMicrostackThreadEx2(j->chain, ILibProcessPipe_Process_OnExit_ChainSink, user, 0);
		return;
	}

	result = GetExitCodeProcess(j->hProcess, &exitCode);
	j->exiting = 1;
	j->exitHandler(j, exitCode, j->userObject);
	j->exiting ^= 1;
	

	if (j->exiting == 0) { ILibProcessPipe_Process_Destroy(j); }

	// We can't destroy this now, because we're on the MicrostackThread. We must destroy this on the WindowsRunLoop Thread.
	//QueueUserAPC((PAPCFUNC)ILibProcessPipe_Process_OnExit_ChainSink_DestroySink, j->parent->workerThread, (ULONG_PTR)j);
}
#ifdef WIN32
void __stdcall ILibProcessPipe_Process_OnExit_ChainSink_APC(ULONG_PTR obj)
{
	ILibProcessPipe_Process_OnExit_ChainSink(NULL, (void*)obj);
}
#endif
BOOL ILibProcessPipe_Process_OnExit(HANDLE event, ILibWaitHandle_ErrorStatus errors, void* user)
{
	ILibProcessPipe_Process_Object* j = (ILibProcessPipe_Process_Object*)user;
	if (errors != ILibWaitHandle_ErrorStatus_NONE) { return(FALSE); }

	UNREFERENCED_PARAMETER(event);
	ILibProcessPipe_WaitHandle_Remove(j->parent, j->hProcess);
	if ((j->stdOut->PAUSED != 0 && j->stdOut->totalRead > 0) || (j->stdErr->PAUSED != 0 && j->stdErr->totalRead > 0))
	{
		j->hProcess_needAdd = 1;
	}
	else
	{
		if (j->exitHandler != NULL)
		{
			// Everyone's lives will be made easier, by context switching to chain thread before making this call
#ifdef WIN32
			QueueUserAPC((PAPCFUNC)ILibProcessPipe_Process_OnExit_ChainSink_APC, ILibChain_GetMicrostackThreadHandle(j->parent->ChainLink.ParentChain), (ULONG_PTR)user);
#else
			ILibChain_RunOnMicrostackThread(j->parent->ChainLink.ParentChain, ILibProcessPipe_Process_OnExit_ChainSink, user);
#endif
		}
		else
		{
			ILibProcessPipe_Process_Destroy(j);
		}
	}
	return(FALSE);
}
#endif
void ILibProcessPipe_Process_UpdateUserObject(ILibProcessPipe_Process module, void *userObj)
{
	((ILibProcessPipe_Process_Object*)module)->userObject = userObj;
}
void ILibProcessPipe_Process_AddHandlers(ILibProcessPipe_Process module, int bufferSize, ILibProcessPipe_Process_ExitHandler exitHandler, ILibProcessPipe_Process_OutputHandler stdOut, ILibProcessPipe_Process_OutputHandler stdErr, ILibProcessPipe_Process_SendOKHandler sendOk, void *user)
{
	ILibProcessPipe_Process_Object* j = (ILibProcessPipe_Process_Object*)module;
	if (j != NULL && ILibMemory_CanaryOK(j))
	{
		j->userObject = user;
		j->exitHandler = exitHandler;

		ILibProcessPipe_Process_StartPipeReader(j->stdOut, bufferSize, &ILibProcessPipe_Process_PipeHandler_StdOut, j, stdOut);
		ILibProcessPipe_Process_StartPipeReader(j->stdErr, bufferSize, &ILibProcessPipe_Process_PipeHandler_StdOut, j, stdErr);
		ILibProcessPipe_Process_SetWriteHandler(j->stdIn, &ILibProcessPipe_Process_PipeHandler_StdIn, j, sendOk);

#ifdef WIN32
		ILibProcessPipe_WaitHandle_Add(j->parent, j->hProcess, j, &ILibProcessPipe_Process_OnExit);
#endif
	}
}
void ILibProcessPipe_Pipe_Close(ILibProcessPipe_Pipe po)
{
	ILibProcessPipe_PipeObject* pipeObject = (ILibProcessPipe_PipeObject*)po;
	if (pipeObject != NULL)
	{
#ifdef WIN32
		CloseHandle(pipeObject->mPipe_WriteEnd);
		pipeObject->mPipe_WriteEnd = NULL;
#else
		close(pipeObject->mPipe_WriteEnd);
		pipeObject->mPipe_WriteEnd = -1;
#endif
	}
}

ILibTransport_DoneState ILibProcessPipe_Pipe_Write(ILibProcessPipe_Pipe po, char* buffer, int bufferLen, ILibTransport_MemoryOwnership ownership)
{
	ILibProcessPipe_PipeObject* pipeObject = (ILibProcessPipe_PipeObject*)po;
	ILibTransport_DoneState retVal = ILibTransport_DoneState_ERROR;

	if (pipeObject == NULL)
	{
		return(ILibTransport_DoneState_ERROR);
	}

	if (pipeObject->WriteBuffer == NULL)
	{
		pipeObject->WriteBuffer = ILibQueue_Create();
	}

	ILibQueue_Lock(pipeObject->WriteBuffer);
	if (ILibQueue_IsEmpty(pipeObject->WriteBuffer) == 0)
	{
		ILibQueue_EnQueue(pipeObject->WriteBuffer, ILibProcessPipe_WriteData_Create(buffer, bufferLen, ownership));
	}
	else
	{
#ifdef WIN32
		BOOL result = WriteFile(pipeObject->mPipe_WriteEnd, buffer, bufferLen, NULL, pipeObject->mOverlapped);
		if (result == TRUE) { retVal = ILibTransport_DoneState_COMPLETE; }
#else
		int result = (int)write(pipeObject->mPipe_WriteEnd, buffer, bufferLen);
		if (result > 0) { retVal = ILibTransport_DoneState_COMPLETE; }
#endif
		else
		{
#ifdef WIN32
			if (GetLastError() == ERROR_IO_PENDING)
#else
			if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
#endif
			{
				retVal = ILibTransport_DoneState_INCOMPLETE;
				ILibQueue_EnQueue(pipeObject->WriteBuffer, ILibProcessPipe_WriteData_Create(buffer, bufferLen, ownership));
#ifdef WIN32
				ILibProcessPipe_WaitHandle_Add(pipeObject->manager, pipeObject->mOverlapped->hEvent, pipeObject, &ILibProcessPipe_Process_WindowsWriteHandler);
#else
				ILibLifeTime_Add(ILibGetBaseTimer(pipeObject->manager->ChainLink.ParentChain), pipeObject, 0, &ILibProcessPipe_Process_StartPipeReaderWriterEx, NULL); // Need to context switch to Chain Thread
#endif
			}
			else
			{
				if (pipeObject->manager != NULL)
				{
#ifdef WIN32
					ILibRemoteLogging_printf(ILibChainGetLogger(pipeObject->manager->ChainLink.ParentChain), ILibRemoteLogging_Modules_Microstack_Pipe, ILibRemoteLogging_Flags_VerbosityLevel_1, "ILibProcessPipe[Write]: BrokenPipe(%d) on Pipe: %p", GetLastError(), (void*)pipeObject);
#else
					ILibRemoteLogging_printf(ILibChainGetLogger(pipeObject->manager->ChainLink.ParentChain), ILibRemoteLogging_Modules_Microstack_Pipe, ILibRemoteLogging_Flags_VerbosityLevel_1, "ILibProcessPipe[Write]: BrokenPipe(%d) on Pipe: %p", result < 0 ? errno : 0, (void*)pipeObject);
#endif
				}
				ILibQueue_UnLock(pipeObject->WriteBuffer);
				if (pipeObject->brokenPipeHandler != NULL)
				{
#ifdef WIN32
					if (pipeObject->manager != NULL)
					{
						ILibProcessPipe_WaitHandle_Remove(pipeObject->manager, pipeObject->mOverlapped->hEvent); // Pipe Broken, so remove ourselves from the processing loop
					}
#endif
					((ILibProcessPipe_GenericBrokenPipeHandler)pipeObject->brokenPipeHandler)(pipeObject);
				}
				ILibProcessPipe_FreePipe(pipeObject);
				return(ILibTransport_DoneState_ERROR);
			}
		}
	}
	ILibQueue_UnLock(pipeObject->WriteBuffer);
	
	return retVal;
}
void ILibProcessPipe_Process_CloseStdIn(ILibProcessPipe_Process p)
{
	ILibProcessPipe_Process_Object *j = (ILibProcessPipe_Process_Object*)p;
	if (ILibMemory_CanaryOK(j))
	{
		ILibProcessPipe_Pipe_Close(j->stdIn);
	}
}
ILibTransport_DoneState ILibProcessPipe_Process_WriteStdIn(ILibProcessPipe_Process p, char* buffer, int bufferLen, ILibTransport_MemoryOwnership ownership)
{
	ILibProcessPipe_Process_Object *j = (ILibProcessPipe_Process_Object*)p;
	if (ILibMemory_CanaryOK(j))
	{
		return(ILibProcessPipe_Pipe_Write(j->stdIn, buffer, bufferLen, ownership));
	}
	else
	{
		return(ILibTransport_DoneState_ERROR);
	}
}

void ILibProcessPipe_Pipe_ReadSink(char *buffer, int bufferLen, int* bytesConsumed, void* user1, void* user2)
{
	ILibProcessPipe_Pipe target = (ILibProcessPipe_Pipe)user1;

	if (user2 != NULL) { ((ILibProcessPipe_Pipe_ReadHandler)user2)(target, buffer, bufferLen, bytesConsumed); }
}
void ILibProcessPipe_Pipe_AddPipeReadHandler(ILibProcessPipe_Pipe targetPipe, int bufferSize, ILibProcessPipe_Pipe_ReadHandler OnReadHandler)
{
	ILibProcessPipe_Process_StartPipeReader(targetPipe, bufferSize, &ILibProcessPipe_Pipe_ReadSink, targetPipe, OnReadHandler);
}
#ifdef WIN32
void __stdcall ILibProcessPipe_Pipe_Read_CompletionRoutine(DWORD dwErrorCode, DWORD dwNumberOfBytesTransfered, LPOVERLAPPED lpOverlapped)
{
	ILibProcessPipe_PipeObject *j = (ILibProcessPipe_PipeObject*)((void**)ILibMemory_GetExtraMemory(lpOverlapped, sizeof(OVERLAPPED)))[0];
	if (!ILibMemory_CanaryOK(j)) { return; }
	
	ILibProcessPipe_Pipe_ReadExHandler callback = (ILibProcessPipe_Pipe_ReadExHandler)j->user2;
	if (callback != NULL) { callback(j, j->user1, dwErrorCode, j->buffer, dwNumberOfBytesTransfered); }
}
void __stdcall ILibProcessPipe_Pipe_Write_CompletionRoutine(DWORD dwErrorCode, DWORD dwNumberOfBytesTransfered, LPOVERLAPPED lpOverlapped)
{
	ILibProcessPipe_PipeObject *j = (ILibProcessPipe_PipeObject*)((void**)ILibMemory_GetExtraMemory(lpOverlapped, sizeof(OVERLAPPED)))[0];
	if (!ILibMemory_CanaryOK(j)) { return; }

	if (j->user4 != NULL)
	{
		((ILibProcessPipe_Pipe_WriteExHandler)j->user4)(j, j->user3, dwErrorCode, dwNumberOfBytesTransfered);
	}
}
int ILibProcessPipe_Pipe_CancelEx(ILibProcessPipe_Pipe targetPipe)
{
	ILibProcessPipe_PipeObject *j = (ILibProcessPipe_PipeObject*)targetPipe;
	if (!ILibMemory_CanaryOK(j) || j->mPipe_ReadEnd == NULL) { return(2); }
	return(CancelIoEx(j->mPipe_ReadEnd, NULL));
}
int ILibProcessPipe_Pipe_ReadEx(ILibProcessPipe_Pipe targetPipe, char *buffer, int bufferLength, void *user, ILibProcessPipe_Pipe_ReadExHandler OnReadHandler)
{
	ILibProcessPipe_PipeObject *j = (ILibProcessPipe_PipeObject*)targetPipe;
	j->usingCompletionRoutine = 1;
	j->buffer = buffer;
	j->bufferSize = bufferLength;
	j->user1 = user;
	j->user2 = OnReadHandler;
	if (!ReadFileEx(j->mPipe_ReadEnd, j->buffer, j->bufferSize, j->mOverlapped, ILibProcessPipe_Pipe_Read_CompletionRoutine))
	{
		return(GetLastError());
	}
	else
	{
		return(0);
	}
}
int ILibProcessPipe_Pipe_WriteEx(ILibProcessPipe_Pipe targetPipe, char *buffer, int bufferLength, void *user, ILibProcessPipe_Pipe_WriteExHandler OnWriteHandler)
{
	ILibProcessPipe_PipeObject *j = (ILibProcessPipe_PipeObject*)targetPipe;
	if (j->mwOverlapped == NULL)
	{
		void **extra;
		j->mwOverlapped = (OVERLAPPED*)ILibMemory_Allocate(sizeof(OVERLAPPED), sizeof(void*), NULL, (void**)&extra);
		extra[0] = j;
	}
	j->user3 = user;
	j->user4 = OnWriteHandler;
	if (!WriteFileEx(j->mPipe_WriteEnd, buffer, bufferLength, j->mwOverlapped, ILibProcessPipe_Pipe_Write_CompletionRoutine))
	{
		return(GetLastError());
	}
	else
	{
		return(0);
	}
}
DWORD ILibProcessPipe_Process_GetPID(ILibProcessPipe_Process p) { return(p != NULL ? (DWORD)((ILibProcessPipe_Process_Object*)p)->PID : 0); }
#else
pid_t ILibProcessPipe_Process_GetPID(ILibProcessPipe_Process p) { return(p != NULL ? (pid_t)((ILibProcessPipe_Process_Object*)p)->PID : 0); }
#endif

