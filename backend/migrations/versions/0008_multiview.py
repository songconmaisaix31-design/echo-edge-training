from alembic import op
import sqlalchemy as sa


revision = "0008"
down_revision = "0007"
branch_labels = None
depends_on = None


def upgrade():
    op.create_table(
        "examination_multiview",
        sa.Column("exam_id", sa.Integer(), sa.ForeignKey("examinations.id"), primary_key=True),
        sa.Column("revision", sa.Integer(), nullable=False),
        sa.Column("data", sa.Text(), nullable=False),
    )


def downgrade():
    op.drop_table("examination_multiview")
