"""Add immutable research report versions."""
from alembic import op
import sqlalchemy as sa

revision = "0006"
down_revision = "0005"


def upgrade():
    op.create_table(
        "research_report_versions",
        sa.Column("id", sa.Integer(), primary_key=True),
        sa.Column("exam_id", sa.Integer(), sa.ForeignKey("examinations.id"), nullable=False),
        sa.Column("review_id", sa.Integer(), sa.ForeignKey("reviews.id"), nullable=False),
        sa.Column("author", sa.Integer(), sa.ForeignKey("users.id"), nullable=False),
        sa.Column("created", sa.DateTime(), nullable=False),
        sa.Column("snapshot", sa.Text(), nullable=False),
    )
    op.create_index("ix_research_report_versions_exam_id", "research_report_versions", ["exam_id"])


def downgrade():
    op.drop_table("research_report_versions")
