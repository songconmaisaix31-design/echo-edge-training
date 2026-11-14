from alembic import op
import sqlalchemy as sa
from exam_parameters import ClinicalVersion, Measurement
revision='0003'
down_revision='0002'
def upgrade():
    columns = {c['name'] for c in sa.inspect(op.get_bind()).get_columns('patients')}
    if 'history' not in columns:
        op.add_column('patients',sa.Column('history',sa.Text(),nullable=False,server_default=''))
    if 'treatment_history' not in columns:
        op.add_column('patients',sa.Column('treatment_history',sa.Text(),nullable=False,server_default=''))
    ClinicalVersion.__table__.create(op.get_bind(),checkfirst=True)
    Measurement.__table__.create(op.get_bind(),checkfirst=True)
def downgrade():
    Measurement.__table__.drop(op.get_bind())
    ClinicalVersion.__table__.drop(op.get_bind())
    op.drop_column('patients','treatment_history')
    op.drop_column('patients','history')
