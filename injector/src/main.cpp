#include <Windows.h>
#include <string>
#include <vector>
#include <TlHelp32.h>

#include "logging/logger_setup.hpp"
#include <glog/logging.h>

// Имя DLL для поиска модуля
static const wchar_t* kDllName = L"wotlk.dll";
static const char* kDllNameA = "wotlk.dll";

// Получение ID процесса (PID) по имени
DWORD GetProcessIdByName(const std::wstring& processName)
{
	DWORD pid = 0;
	HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (hSnapshot != INVALID_HANDLE_VALUE)
	{
		PROCESSENTRY32W pe;
		pe.dwSize = sizeof(pe);
		if (Process32FirstW(hSnapshot, &pe))
		{
			do
			{
				if (processName == pe.szExeFile)
				{
					pid = pe.th32ProcessID;
					break;
				}
			} while (Process32NextW(hSnapshot, &pe));
		}
		CloseHandle(hSnapshot);
	}
	return pid;
}

// Поиск модуля (DLL) в процессе по имени, возвращает HMODULE или NULL
HMODULE FindRemoteModule(DWORD pid, const std::wstring& moduleName)
{
	HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
	if (hSnapshot == INVALID_HANDLE_VALUE) return nullptr;

	MODULEENTRY32W me;
	me.dwSize = sizeof(me);
	HMODULE result = nullptr;

	if (Module32FirstW(hSnapshot, &me))
	{
		do
		{
			if (_wcsicmp(me.szModule, moduleName.c_str()) == 0)
			{
				result = me.hModule;
				break;
			}
		} while (Module32NextW(hSnapshot, &me));
	}
	CloseHandle(hSnapshot);
	return result;
}

// Выгрузка DLL из процесса через named event
// DLL сама делает cleanup и вызывает FreeLibraryAndExitThread
static const char* kUnloadEventName = "wotlk_unload_event";

bool EjectDLL(DWORD pid)
{
	HMODULE hRemoteDll = FindRemoteModule(pid, kDllName);
	if (!hRemoteDll)
	{
		LOG(WARNING) << "DLL not found in target process. Nothing to eject.";
		return false;
	}

	LOG(INFO) << "Found module at 0x" << std::hex << (DWORD)hRemoteDll << std::dec;

	// Сигнал DLL через named event — она сама сделает cleanup и самовыгрузку
	HANDLE hEvent = OpenEventA(EVENT_MODIFY_STATE, FALSE, kUnloadEventName);
	if (!hEvent)
	{
		LOG(ERROR) << "Failed to open unload event. Error: " << GetLastError();
		LOG(ERROR) << "Is the DLL loaded and running?";
		return false;
	}

	SetEvent(hEvent);
	CloseHandle(hEvent);
	LOG(INFO) << "Unload signal sent. Waiting for DLL to unload...";

	// Подождать пока DLL выгрузится (макс 5 секунд)
	for (int i = 0; i < 50; ++i)
	{
		Sleep(100);
		if (FindRemoteModule(pid, kDllName) == nullptr)
		{
			LOG(INFO) << "DLL ejected successfully.";
			return true;
		}
	}

	LOG(ERROR) << "Timeout: DLL did not unload within 5 seconds.";
	return false;
}

// функция инжекции с получением GetLastError из удалённого процесса
bool InjectDLL(DWORD pid, const std::string& dllPath)
{
	HANDLE hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
	if (!hProcess)
	{
		LOG(ERROR) << "Failed to open process. Error: " << GetLastError() << ". Run as Admin?";
		return false;
	}

	HMODULE hKernel32 = GetModuleHandleA("kernel32.dll");
	auto pLoadLibraryEx = (DWORD)GetProcAddress(hKernel32, "LoadLibraryExA");
	auto pGetLastError = (DWORD)GetProcAddress(hKernel32, "GetLastError");

	// Структура данных для удалённого кода:
	// [0]  pLoadLibraryExA
	// [4]  pGetLastError
	// [8]  hModule (результат)
	// [12] lastError (результат)
	// [16] dllPath[]
	const DWORD kHeaderSize = 16;
	DWORD dataSize = kHeaderSize + (DWORD)dllPath.size() + 1;

	// x86 shellcode (stdcall):
	// Вызывает LoadLibraryExA(dllPath, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)
	// 0x1100 = LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR (0x100) | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS (0x1000)
	// Явно указывает Windows: искать зависимости в папке DLL + системных директориях
	unsigned char shellcode[] = {
		0x8B, 0x5C, 0x24, 0x04,              // mov ebx, [esp+4]
		0x68, 0x00, 0x11, 0x00, 0x00,        // push 0x1100
		0x6A, 0x00,                           // push 0 (hFile = NULL)
		0x8D, 0x43, 0x10,        // lea eax, [ebx+16]
		0x50,                    // push eax
		0xFF, 0x13,              // call [ebx] (LoadLibraryExA)
		0x89, 0x43, 0x08,        // mov [ebx+8], eax
		0x85, 0xC0,              // test eax, eax
		0x75, 0x06,              // jnz +6
		0xFF, 0x53, 0x04,        // call [ebx+4] (GetLastError)
		0x89, 0x43, 0x0C,        // mov [ebx+12], eax
		0xC2, 0x04, 0x00         // ret 4
	};
	DWORD totalSize = dataSize + sizeof(shellcode);

	// Выделить память под данные + код (RWX)
	void* pRemote = VirtualAllocEx(hProcess, nullptr, totalSize, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
	if (!pRemote)
	{
		LOG(ERROR) << "Failed to allocate memory in target process.";
		CloseHandle(hProcess);
		return false;
	}

	// Заполнить структуру данных
	std::vector<BYTE> buffer(totalSize, 0);
	memcpy(buffer.data() + 0, &pLoadLibraryEx, 4);
	memcpy(buffer.data() + 4, &pGetLastError, 4);
	memcpy(buffer.data() + kHeaderSize, dllPath.c_str(), dllPath.size() + 1);
	memcpy(buffer.data() + dataSize, shellcode, sizeof(shellcode));

	WriteProcessMemory(hProcess, pRemote, buffer.data(), totalSize, nullptr);

	// Запустить shellcode, передав указатель на данные
	auto pCode = (LPTHREAD_START_ROUTINE)((BYTE*)pRemote + dataSize);
	HANDLE hThread = CreateRemoteThread(hProcess, nullptr, 0, pCode, pRemote, 0, nullptr);

	if (!hThread)
	{
		LOG(ERROR) << "Remote thread creation failed. Error: " << GetLastError();
		VirtualFreeEx(hProcess, pRemote, 0, MEM_RELEASE);
		CloseHandle(hProcess);
		return false;
	}

	WaitForSingleObject(hThread, INFINITE);
	CloseHandle(hThread);

	// Считать результаты из удалённой памяти
	DWORD results[2] = {}; // [0]=hModule, [1]=lastError
	ReadProcessMemory(hProcess, (BYTE*)pRemote + 8, results, sizeof(results), nullptr);

	VirtualFreeEx(hProcess, pRemote, 0, MEM_RELEASE);
	CloseHandle(hProcess);

	if (results[0] == 0)
	{
		LOG(ERROR) << "LoadLibraryA failed inside target process.";
		LOG(ERROR) << "Remote GetLastError: " << results[1];

		switch (results[1])
		{
		case 126: LOG(ERROR) << "ERROR_MOD_NOT_FOUND (126): DLL or one of its dependencies not found."; break;
		case 193: LOG(ERROR) << "ERROR_BAD_EXE_FORMAT (193): DLL architecture mismatch (x86 vs x64)."; break;
		case   5: LOG(ERROR) << "ERROR_ACCESS_DENIED (5): Access denied."; break;
		case  87: LOG(ERROR) << "ERROR_INVALID_PARAMETER (87): Invalid parameter."; break;
		default:  LOG(ERROR) << "Look up error code at: https://learn.microsoft.com/en-us/windows/win32/debug/system-error-codes"; break;
		}
		return false;
	}

	LOG(INFO) << "DLL injected successfully! Module handle: 0x" << std::hex << results[0];
	return true;
}

int main(int argc, char** argv)
{
	logger::Initialize({"injector", "injector", "./logs", true, true});

	bool ejectMode = false;
	for (int i = 1; i < argc; ++i)
	{
		if (strcmp(argv[i], "--eject") == 0 || strcmp(argv[i], "-e") == 0)
			ejectMode = true;
	}

	LOG(INFO) << (ejectMode ? "Mode: EJECT" : "Mode: INJECT");
	LOG(INFO) << "Looking for Wow.exe process...";

	DWORD pid = 0;
	while ((pid = GetProcessIdByName(L"Wow.exe")) == 0)
	{
		Sleep(1000);
	}
	LOG(INFO) << "Found WoW PID: " << pid;

	if (ejectMode)
	{
		EjectDLL(pid);
		logger::Shutdown();
		return 0;
	}

	// --- Inject mode ---
	char exePath[MAX_PATH];
	GetModuleFileNameA(nullptr, exePath, MAX_PATH);
	std::string dllPath(exePath);
	dllPath = dllPath.substr(0, dllPath.find_last_of("\\/") + 1) + kDllNameA;

	LOG(INFO) << "Injecting DLL: " << dllPath;

	DWORD fileAttr = GetFileAttributesA(dllPath.c_str());
	if (fileAttr == INVALID_FILE_ATTRIBUTES)
	{
		LOG(ERROR) << "DLL file not found: " << dllPath;
		logger::Shutdown();
		return 1;
	}

	// Проверка архитектуры целевого процесса
	HANDLE hCheck = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (hCheck)
	{
		BOOL isWow64 = FALSE;
		if (IsWow64Process(hCheck, &isWow64))
		{
			if (!isWow64)
			{
				LOG(ERROR) << "Wow.exe is a 64-bit process, but the DLL is 32-bit. Injection impossible.";
				CloseHandle(hCheck);
				logger::Shutdown();
				return 1;
			}
			LOG(INFO) << "Architecture check passed (both x86).";
		}
		CloseHandle(hCheck);
	}

	if (InjectDLL(pid, dllPath))
	{
		LOG(INFO) << "Done. You can now close this injector.";
	}

	logger::Shutdown();
	return 0;
}
