; // Event log messages for the WinOomKiller event source. Each event carries
; // its full text as the one insertion string; the ids only let the event
; // viewer render it and let admins filter by severity. Registered by the
; // installers as the source's EventMessageFile (the service exe itself).

MessageIdTypedef=DWORD

LanguageNames=(English=0x409:MSG00409)

MessageId=1
Severity=Success
SymbolicName=MSG_INFORMATION
Language=English
%1%0
.

MessageId=2
Severity=Success
SymbolicName=MSG_WARNING
Language=English
%1%0
.

MessageId=3
Severity=Success
SymbolicName=MSG_ERROR
Language=English
%1%0
.
