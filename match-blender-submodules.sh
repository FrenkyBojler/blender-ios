path=$PWD

submodules=$(git config --file .gitmodules --get-regexp path | awk '{ print $2 }')

for submodule in $submodules; do :

rev_info=$(git ls-tree main $submodule)
rev_data=($rev_info)
rev=${rev_data[2]}
echo $submodule in  $rev

cd  $submodule
git checkout $rev
cd $path

done

