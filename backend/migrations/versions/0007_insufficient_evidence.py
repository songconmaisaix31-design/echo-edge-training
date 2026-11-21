from alembic import op
import sqlalchemy as sa


revision = "0007"
down_revision = "0006"
branch_labels = None
depends_on = None


def upgrade():
    with op.batch_alter_table("analysis_jobs") as batch:
        batch.alter_column("state", existing_type=sa.String(16), type_=sa.String(32), existing_nullable=False)


def downgrade():
    op.execute(
        "UPDATE analysis_jobs SET state = 'failed', "
        "error = '所选帧未确认属于心尖四腔心切面。' "
        "WHERE state = 'insufficient_evidence' AND model_type = 'cardiacudc_fourchamber'"
    )
    op.execute("UPDATE analysis_jobs SET state = 'succeeded' WHERE state = 'insufficient_evidence'")
    with op.batch_alter_table("analysis_jobs") as batch:
        batch.alter_column("state", existing_type=sa.String(32), type_=sa.String(16), existing_nullable=False)
