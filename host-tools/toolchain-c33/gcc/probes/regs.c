extern int g(int);
int use_many(int a) {
  int v0=g(a),v1=g(a+1),v2=g(a+2),v3=g(a+3),v4=g(a+4),v5=g(a+5),v6=g(a+6),v7=g(a+7);
  return v0+v1+v2+v3+v4+v5+v6+v7;
}
int leaf(int a){ return a*3; }
int frame(int a){ volatile int buf[8]; buf[0]=a; return buf[0]+buf[7]; }
