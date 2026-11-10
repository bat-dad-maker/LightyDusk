import subprocess,json,concurrent.futures,pathlib,math,argparse,shlex
parser=argparse.ArgumentParser()
parser.add_argument("--lifecycle",action="store_true",help="pause reliable gameplay events until sync is received")
parser.add_argument("--delay-ms",type=int,default=25,help="one-way link delay")
args=parser.parse_args()
if args.delay_ms < 0: parser.error("delay must be nonnegative")
base=pathlib.Path(__file__).resolve().parents[2]
b=base/'build/latency-benchmark'
b.mkdir(parents=True,exist_ok=True)
# Requires Linux g++, zstd headers/library, iproute2, ethtool and root for
# disposable network namespaces. Does not alter the host network or launch Dusk.
baseline='39e7ade2a9dfd9db95ba8bf0e5d575c710e7c419'
(b/'tcp_transport.cpp').write_bytes(subprocess.check_output(['git','show',baseline+':src/net/transport.cpp'],cwd=base))
common=['g++','-std=c++20','-Wl,--wrap=sendto','-O2','-pthread','-Iinclude','-Ibuild-upstream-msvc-release/_deps/nlohmann_json-src/include']
sources=['tests/reliable_udp/transport_latency_benchmark.cpp','src/net/udp_codec.cpp','src/net/invite_code.cpp']
subprocess.run(common+sources+[str(b/'tcp_transport.cpp'),'-lzstd','-o',str(b/'tcp_benchmark')],cwd=base,check=True)
subprocess.run(['gcc','-O2','-c','vendor/kcp/ikcp.c','-o',str(b/'kcp_linux.o')],cwd=base,check=True)
subprocess.run(common+['-Ivendor/kcp']+sources+['src/net/transport.cpp','src/net/reliable_udp.cpp','src/net/datagram_scheduler.cpp','src/net/udp_connection.cpp',str(b/'kcp_linux.o'),'-lzstd','-o',str(b/'kcp_benchmark')],cwd=base,check=True)
# Deliberately overfilled schema: all progression slots, including excluded
# values, and padded compressed-state allowance. NOT a captured player save.

def run(case):
 protocol,loss,bulk,seed=case
 payload=str(base/'tests/reliable_udp/full_progression_upper.json') if bulk else 'none'
 cmd='set -eu; ip link set lo mtu 1200 up; ethtool -K lo tso off gso off gro off >/dev/null 2>&1; tc qdisc add dev lo root netem limit 10000 delay '+str(args.delay_ms)+'ms loss random '+str(loss)+'% rate 4mbit seed '+str(seed)+'; exec '+shlex.quote(str(b/(protocol+'_benchmark')))+' '+shlex.quote(payload)+(' lifecycle' if args.lifecycle else '')
 r=subprocess.run(['unshare','-n','bash','-c',cmd],cwd=base,capture_output=True,text=True,timeout=110)
 if r.returncode: raise RuntimeError(str(case)+': '+r.stderr+r.stdout)
 row=dict(protocol=protocol,loss=loss,bulk=bulk,seed=seed,lifecycle=args.lifecycle,one_way_ms=args.delay_ms,**json.loads(r.stdout))
 print('Completed',protocol,loss,bulk,seed,flush=True)
 return row
cases=[(p,l,b,s) for l in [0,2,5,10] for b in [False,True] for p in ['tcp','kcp'] for s in [1,2,3]]
with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool: rows=list(pool.map(run,cases))
(b/'comparison_results.json').write_text(json.dumps(rows,indent=2))
print('protocol,loss,bulk,payload_bytes,n,median_ms,p95_ms,max_ms')
for loss in [0,2,5,10]:
 for bulk in [False,True]:
  for p in ['tcp','kcp']:
   matches=[r for r in rows if (r['protocol'],r['loss'],r['bulk'])==(p,loss,bulk)]
   a=sorted(v for r in matches for v in r['latencies'])
   print(p,loss,bulk,matches[0]['payload_bytes'],len(a),a[math.ceil(len(a)*.5)-1],a[math.ceil(len(a)*.95)-1],a[-1],sep=',')
