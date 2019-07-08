# http.server does not support status codes besides 200
echo Content-Type: text/html
echo
echo "<html><head><meta http-equiv=refresh content=\"0; $*\"></head></html>"
