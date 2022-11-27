# OS_multithreading
## Intro 
A program that searches a directory tree for files whose name matches some search term.<br />
The program receives a directory D and a search term T, and finds every file<br />
in D’s directory tree whose name contains T.<br />
The program parallelizes its work using threads.<br />
Specifically, individual directories are searched by different threads.<br />
