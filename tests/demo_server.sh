query() {
  read -p "$1 (Y/N)? " -n 1 -r
  echo ""
  if [[ $REPLY =~ ^[Yy]$ ]]; then
    RTN="$3"
  else
    RTN=""
  fi
  eval $2=\"$RTN\"
}

#query "Close first client after 175 msgs" m_opt "m 175"

ci_opt=""
if [ "$m_opt" == "" ]; then
  query "Close inactive client on notification" ci_opt "ci"
fi

query "Exit after 8 \"Waiting ...\" messages" i_opt "i 8"


../build/tests/cimpmsg_test_server p 6666 $ci_opt $i_opt

