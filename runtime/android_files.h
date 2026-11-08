// Android: the app's files folder (runtime/platform/android.c; the settings' Android tab shows it)
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
const char *android_home(void);                   // the files folder in use (the working directory)
const char *android_files_folder(void);           // the one the player chose, "" when it is the app's own
const char *android_files_folder_problem(void);   // why a chosen one isn't used now, or ""
#ifdef __cplusplus
}
#endif
