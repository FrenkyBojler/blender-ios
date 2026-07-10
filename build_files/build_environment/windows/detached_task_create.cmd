:: C:\db\detached_task_create.cmd
set /p USER_PASSWORD=Enter password: 
schtasks /create /tn BuildJob /tr "C:\db\detached_build.cmd" /sc once /st 00:00 /ru %USERNAME% /rp %USER_PASSWORD% /f
