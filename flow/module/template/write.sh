echo bar > resource
chmod +x '@OUT(store.plan,out/)bin/save'
'@OUT(store.plan,out/)bin/save' resource
