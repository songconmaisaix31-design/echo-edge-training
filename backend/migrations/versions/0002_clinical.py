from alembic import op
from clinical import Entry, Examination, Patient

revision = '0002'
down_revision = '0001'
def upgrade():
    for table in (Patient.__table__, Examination.__table__, Entry.__table__):
        table.create(op.get_bind(), checkfirst=True)
def downgrade():
    for table in (Entry.__table__, Examination.__table__, Patient.__table__):
        table.drop(op.get_bind())
