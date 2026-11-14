import os, subprocess, sys, sqlite3, tempfile
from pathlib import Path
root=Path(__file__).resolve().parent
for mode in ['fresh','legacy']:
 folder=Path(tempfile.mkdtemp(prefix='echo-patient-migration-'));db=folder/'test.db'
 if mode=='legacy':
  conn=sqlite3.connect(db)
  conn.executescript("CREATE TABLE alembic_version (version_num VARCHAR(32) PRIMARY KEY); INSERT INTO alembic_version VALUES ('0003'); CREATE TABLE users(id INTEGER PRIMARY KEY); INSERT INTO users VALUES (1); CREATE TABLE patients(id INTEGER PRIMARY KEY,owner INTEGER NOT NULL REFERENCES users(id),name VARCHAR(120) NOT NULL,birth VARCHAR(10) NOT NULL,sex VARCHAR(16) NOT NULL,notes TEXT NOT NULL,history TEXT NOT NULL,treatment_history TEXT NOT NULL); INSERT INTO patients VALUES (7,1,'Synthetic legacy','2020-01-01','unknown','notes','history','treatment');")
  conn.commit();conn.close()
 env={**os.environ,'ECHO_DATABASE_URL':'sqlite:///'+str(db)}
 subprocess.run([sys.executable,'-m','alembic','upgrade','head'],cwd=root/'backend',env=env,check=True)
 conn=sqlite3.connect(db)
 assert conn.execute('select version_num from alembic_version').fetchone()[0]=='0004'
 if mode=='legacy':
  row=conn.execute('select id,history,treatment_history,business_number,created from patients').fetchone()
  assert row[:3]==(7,'history','treatment') and row[3].startswith('EE-') and row[4] is None
 conn.close()
 print(mode+' migration passed')
