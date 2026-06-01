@echo off
setlocal enabledelayedexpansion
set "base=%CD%"
(for /R %%i in (*) do (
    set "full=%%i"
    set "rel=!full:%base%\=!"
    echo !rel!
)) > _files_mp_.txt