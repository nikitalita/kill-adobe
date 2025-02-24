#include <Windows.h>
#include <TlHelp32.h> /* PROCESSENTRY32 */
#include <string.h>
#include <stdbool.h>
#include <wchar.h>

#define PREDEF_PROC_MAX_IDX 18

#ifndef NDEBUG
#include <stdio.h>
#endif

/* error codes */
#define ERR_STRING 1
#define ERR_CLOSE 2
#define ERR_ALLOC 3
#define ERR_HANDLE 4

BOOL CALLBACK h_hwnd_of_adobe_proc(HWND window, LPARAM proc);
WNDENUMPROC hwnd_of_adobe_proc = &h_hwnd_of_adobe_proc; /* I don't want to know whether MSVC allows the K&R decl reqd for direct definition */
bool ok_to_kill;

int main(int argc, char* argv[])
{
	/* declarations */
	PROCESSENTRY32 process;
	HANDLE sys_snapshot = 0;
	HANDLE token_handle = 0;
	TOKEN_PRIVILEGES privilege,
		old_privilege;
	bool hit;
	size_t i,
		proc_name_ct;
	wchar_t** proc_names;
	int status;
	DWORD wait_length,
		old_privilege_sz; /* this is necessary only so AdjustTokenPrivilege doesn't freak out; find a way to remove */

	wait_length = 10000; /* check back every 10s by default */
	process.dwSize = sizeof(PROCESSENTRY32); /* for the API call that generates a list */

	/* create process name array */
	proc_name_ct = argc + PREDEF_PROC_MAX_IDX;
	if (!(proc_names = calloc(proc_name_ct, sizeof(wchar_t)))) /* calloc so that frees are valid in case string transfer is aborted */
	{
		status = ERR_ALLOC;
		goto cleanup;
	}
	
	proc_names[0] = L"AdobeUpdateService.exe";
	proc_names[1] = L"AGSService.exe";
	proc_names[2] = L"armsvc.exe";
	proc_names[3] = L"CoreSync.exe";
	proc_names[4] = L"Adobe Desktop Process.exe";
	proc_names[5] = L"Adobe Installer.exe";
	proc_names[6] = L"AdobeIPCBroker.exe";
	proc_names[7] = L"CCXProcess.exe";
	proc_names[8] = L"Creative Cloud.exe";
	proc_names[9] = L"Adobe CEF Helper.exe";
	proc_names[10] = L"Adobe Desktop Service.exe";
	proc_names[11] = L"CCLibrary.exe";
	proc_names[12] = L"Creative Cloud Helper.exe";
	proc_names[13] = L"AdobeIPCBroker.exe";
	proc_names[14] = L"AdobeCollabSync.exe";
	proc_names[15] = L"acrotray.exe";
	proc_names[16] = L"AcrobatNotificationClient.exe";
	proc_names[17] = L"AdobeNotificationClient.exe";
	proc_names[18] = L"FNPLicensingService64.exe";

	for (i = PREDEF_PROC_MAX_IDX + 1; i < proc_name_ct; i++)
	{
		size_t len;

		len = strlen(argv[i - PREDEF_PROC_MAX_IDX]) + 1;
		if (!(proc_names[i] = malloc(len * sizeof(wchar_t))))
		{
			status = ERR_ALLOC;
			goto cleanup;
		}
		if ((len - 1) != mbstowcs(proc_names[i], argv[i - PREDEF_PROC_MAX_IDX], len))
		{
			status = ERR_STRING;
			goto cleanup;
		}
	}

	/* reduce thread priority because the whole point of this application is to reduce CPU load */
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);

	/* hide terminal window */
	#ifdef NDEBUG
	ShowWindow(GetConsoleWindow(), SW_HIDE);
	#endif

	/* get thread permission info
	 * if this fails the the permission can't be elevated, but that won't necessarily break everything, so just jump */
	if (!OpenThreadToken(GetCurrentThread(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, false, &token_handle))
	{
		#ifndef NDEBUG
		fprintf(stderr, "Could not get thread token, system error %lu\n", GetLastError());
		#endif
		if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token_handle))
		{
			#ifndef NDEBUG
			fprintf(stderr, "Could not get process token either, system error%lu\n", GetLastError());
			#endif
			goto loop;
		}
		#ifndef NDEBUG
		puts("Fell back to process token and it worked");
		#endif
	}
	if (!(ImpersonateSelf(SecurityImpersonation) && OpenThreadToken(GetCurrentThread(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, false, &token_handle)))
	{
		#ifndef NDEBUG
		fputs("Could not impersonate self\n", stderr);
		#endif
		goto loop;
	}
	if (!LookupPrivilegeValue(NULL, SE_DEBUG_NAME, &(privilege.Privileges[0].Luid))) /* this is fine because the struct owns an array of length 1 by default */
	{
		#ifndef NDEBUG
		fputs("Could not get privilege value\n", stderr);
		#endif
		goto loop;
	}
	/* try to set the thread permission level */
	privilege.PrivilegeCount = 1;
	privilege.Privileges[0].Attributes = 1;
	AdjustTokenPrivileges(token_handle, false, &privilege, sizeof(TOKEN_PRIVILEGES), &old_privilege, &old_privilege_sz);
	if (GetLastError()) /* if this fails the program won't necessarily break so just skip ahead */
	{
		#ifndef NDEBUG
		fputs("First privilege set pass failed\n", stderr);
		#endif
		goto loop;
	}
	old_privilege.PrivilegeCount = 1; /* for some reason a second pass is needed with what seem to be equivalent args */
	old_privilege.Privileges[0].Luid = privilege.Privileges[0].Luid;
	old_privilege.Privileges[0].Attributes |= SE_PRIVILEGE_ENABLED;
	AdjustTokenPrivileges(token_handle, false, &old_privilege, old_privilege_sz, NULL, NULL);
	if (GetLastError()) /* if this fails the program won't necessarily break so just skip ahead */
	{
		#ifndef NDEBUG
		fputs("Second privilege set pass failed\n", stderr);
		#endif
		goto loop;
	}

	/* exist forever */
loop:
	while (true)
	{
		/* get system process snapshot */
		if ((sys_snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)) == INVALID_HANDLE_VALUE)
		{
			status = ERR_HANDLE;
			goto cleanup;
		}

		/* reset hit flag */
		hit = false;

		/* reset ok to kill flag */
		ok_to_kill = true;
		if (Process32First(sys_snapshot, &process)) do for(i = 0; i < proc_name_ct; i++) if (!wcscmp(process.szExeFile, proc_names[i]))
		{
			/* if a window is found, skip ahead to waiting phase */
			EnumWindows(hwnd_of_adobe_proc, (LPARAM)(&process));
			if (!ok_to_kill)
				goto pause;
		} while (Process32Next(sys_snapshot, &process));

		/* get system process snapshot again */
		if ((sys_snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)) == INVALID_HANDLE_VALUE)
		{
			status = ERR_HANDLE;
			goto cleanup;
		}

		/* for each process
		 * for each element in the name list
		 * if match */
		if (Process32First(sys_snapshot, &process)) do for (i = 0; i < proc_name_ct; i++) if (!wcscmp(process.szExeFile, proc_names[i]))
		{
			#ifndef NDEBUG
			if (!process.th32ProcessID)
				puts("System Process found");

			wprintf(L"Trying to close %s with PID %lu: ", process.szExeFile, process.th32ProcessID);
			#endif

			/* terminate */
			HANDLE process_handle;
			if (!(process_handle = OpenProcess(PROCESS_ALL_ACCESS, false, process.th32ProcessID)))
			{
				#ifndef NDEBUG
				fprintf(stderr, "Task handle could not be obtained; system error code %lu\n", GetLastError());
				#endif
				status = ERR_HANDLE;
				goto cleanup;
			}
			if (!TerminateProcess(process_handle, 1))
			{
				#ifndef NDEBUG
				fprintf(stderr, "Task termination failed with system error code %lu\n", GetLastError());
				#endif
				status = ERR_CLOSE;
				goto cleanup;
			}
			#ifndef NDEBUG
			printf("Terminated task successfully\n");
			#endif
			CloseHandle(process_handle);

			/* set hit flag */
			hit = true;
		} while (Process32Next(sys_snapshot, &process));

pause:
		/* if hit flag was set */
		if (hit && wait_length > 1000) wait_length -= 1000; /* process came back before check, so reduce wait time slightly */
		else if (!hit && wait_length < 120000) wait_length += 1000; /* no process came back before check, so wait longer next time */

		#ifndef NDEBUG
		printf("Task(s) %sfound; waiting %lu ms before trying again\n", hit ? "" : "not ", wait_length);
		#endif

		/* don't spaz out */
		Sleep(wait_length);
	}

	/* errors only; there is no such thing as a successful exit */
cleanup:
	for (i = 3; i < proc_name_ct; i++)
		free(proc_names[i]);
	/* free(proc_names); /* Somehow an issue with MSVC calloc causes free() to hang the program; just let the system do my job if it won't let me */

	if (sys_snapshot && !GetHandleInformation(sys_snapshot, 0)) CloseHandle(sys_snapshot);
	if (token_handle && !GetHandleInformation(token_handle, 0)) CloseHandle(token_handle);

	return status;
}

BOOL CALLBACK h_hwnd_of_adobe_proc(HWND window, LPARAM proc)
{
	DWORD window_pid;
	GetWindowThreadProcessId(window, &window_pid);
	
	ok_to_kill = !(IsWindowVisible(window) && (((PPROCESSENTRY32)proc)->th32ProcessID == window_pid));

	if (!ok_to_kill)
	{
		WCHAR buf[128];
		if (!GetWindowText(window, buf, 128))
		{
			DWORD err;
			if (!(err = GetLastError()))
				ok_to_kill = true;
			#ifndef NDEBUG
			else
				wsprintf(buf, L"ERROR: %lu", err);
			#endif
		}
		#ifndef NDEBUG
		wprintf(L"Found window (%ws) associated with process %lu (%ws)%ws\n", buf, window_pid, ((PPROCESSENTRY32)proc)->szExeFile, ok_to_kill ? L", but it is actually invisible" : L"");
		#endif
	}

	/* ok to kill if the window is invisible or unrelated to the process */
	return ok_to_kill; /* this is ok because the caller will break on the first return of false */
}