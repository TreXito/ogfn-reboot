@echo off
C:\ogfn\mitm\Scripts\mitmdump.exe -s C:\ogfn\ogfn_redirect.py --listen-host 127.0.0.1 --listen-port 8980 --set block_global=false --set connection_strategy=lazy > C:\ogfn\proxy.log 2>&1
