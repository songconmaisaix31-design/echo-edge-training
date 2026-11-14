"""Add patient business identity, structured history and revision storage."""
from alembic import op
import sqlalchemy as sa
import secrets
revision = '0004'
down_revision = '0003'

def upgrade():
    columns = {c['name'] for c in sa.inspect(op.get_bind()).get_columns('patients')}
    for column in [sa.Column('business_number', sa.String(40), nullable=True), sa.Column('created', sa.String(40), nullable=True), sa.Column('clinical_data', sa.Text(), nullable=False, server_default='{}')]:
        if column.name not in columns:
            op.add_column('patients', column)
    connection = op.get_bind()
    patients = sa.table('patients', sa.column('id', sa.Integer), sa.column('business_number', sa.String))
    for key in connection.execute(sa.select(patients.c.id).where(patients.c.business_number.is_(None))).scalars():
        connection.execute(patients.update().where(patients.c.id == key).values(business_number='EE-'+secrets.token_hex(6).upper()))
    with op.batch_alter_table('patients') as batch:
        batch.alter_column('business_number', existing_type=sa.String(40), nullable=False)
        if not any(c['column_names'] == ['business_number'] for c in sa.inspect(connection).get_unique_constraints('patients')):
            batch.create_unique_constraint('uq_patients_business_number', ['business_number'])
    if not sa.inspect(connection).has_table('patient_revisions'):
        op.create_table('patient_revisions', sa.Column('id',sa.Integer(),primary_key=True),sa.Column('patient_id',sa.Integer(),sa.ForeignKey('patients.id'),nullable=False),sa.Column('data',sa.Text(),nullable=False),sa.Column('author',sa.Integer(),sa.ForeignKey('users.id'),nullable=False),sa.Column('created',sa.String(40),nullable=False))
        op.create_index('ix_patient_revisions_patient_id','patient_revisions',['patient_id'])

def downgrade():
    raise RuntimeError('Patient archive migration is forward-only to protect medical history.')
